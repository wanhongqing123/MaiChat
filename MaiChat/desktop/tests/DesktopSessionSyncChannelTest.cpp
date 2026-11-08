// Local TLS WebSocket coverage for Desktop account-token renewal.
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509v3.h>

#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <websocketpp/server.hpp>
#include <websocketpp/config/asio.hpp>

#include "MaiThread.h"
#include <memory>
#include <utility>

#include "agent/DesktopSessionSyncChannel.h"

namespace {

struct TestIdentity {
    QByteArray certificate;
    QByteArray privateKey;
};

TestIdentity createTestIdentity() {
    using KeyContext = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
    using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;
    using Extension = std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)>;
    using Buffer = std::unique_ptr<BIO, decltype(&BIO_free)>;
    KeyContext context(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    if (!context || EVP_PKEY_keygen_init(context.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) <= 0)
        return {};
    EVP_PKEY* rawKey = nullptr;
    if (EVP_PKEY_keygen(context.get(), &rawKey) <= 0) return {};
    Key key(rawKey, EVP_PKEY_free);
    Certificate certificate(X509_new(), X509_free);
    if (!certificate || X509_set_version(certificate.get(), 2) <= 0 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) <= 0 ||
        !X509_gmtime_adj(X509_get_notBefore(certificate.get()), -3600) ||
        !X509_gmtime_adj(X509_get_notAfter(certificate.get()), 24 * 3600) ||
        X509_set_pubkey(certificate.get(), key.get()) <= 0)
        return {};
    X509_NAME* name = X509_get_subject_name(certificate.get());
    if (!name ||
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("localhost"), -1,
                                   -1, 0) <= 0 ||
        X509_set_issuer_name(certificate.get(), name) <= 0)
        return {};
    X509V3_CTX extensionContext;
    X509V3_set_ctx(&extensionContext, certificate.get(), certificate.get(), nullptr, nullptr, 0);
    const auto addExtension = [&](int type, const char* value) {
        QByteArray writableValue(value);
        Extension extension(X509V3_EXT_conf_nid(nullptr, &extensionContext, type, writableValue.data()),
                            X509_EXTENSION_free);
        return extension && X509_add_ext(certificate.get(), extension.get(), -1) > 0;
    };
    if (!addExtension(NID_basic_constraints, "critical,CA:TRUE") ||
        !addExtension(NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1") ||
        X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0)
        return {};
    Buffer certificateBuffer(BIO_new(BIO_s_mem()), BIO_free);
    Buffer keyBuffer(BIO_new(BIO_s_mem()), BIO_free);
    if (!certificateBuffer || !keyBuffer || PEM_write_bio_X509(certificateBuffer.get(), certificate.get()) <= 0 ||
        PEM_write_bio_PrivateKey(keyBuffer.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) <= 0)
        return {};
    BUF_MEM* certificateBytes = nullptr;
    BUF_MEM* keyBytes = nullptr;
    BIO_get_mem_ptr(certificateBuffer.get(), &certificateBytes);
    BIO_get_mem_ptr(keyBuffer.get(), &keyBytes);
    if (!certificateBytes || !keyBytes) return {};
    return {QByteArray(certificateBytes->data, int(certificateBytes->length)),
            QByteArray(keyBytes->data, int(keyBytes->length))};
}

// The fixture runs a real TLS WebSocket endpoint on a loopback-only listener.
// Its private worker owns all I/O; accessors synchronize observations with Qt.
class LocalWssServer final {
public:
    LocalWssServer(QByteArray certificate, QByteArray privateKey)
        : certificate_(std::move(certificate)), privateKey_(std::move(privateKey)) {}
    ~LocalWssServer() {
        server_.stop();
        if (worker_.joinable()) worker_.join();
    }

    bool start() {
        server_.clear_access_channels(websocketpp::log::alevel::all);
        server_.clear_error_channels(websocketpp::log::elevel::all);
        server_.init_asio();
        server_.set_max_message_size(96 * 1024);
        server_.set_tls_init_handler([this](websocketpp::connection_hdl) {
            auto context = websocketpp::lib::make_shared<asio::ssl::context>(asio::ssl::context::tls_server);
            context->use_certificate_chain(asio::buffer(certificate_.constData(), certificate_.size()));
            context->use_private_key(asio::buffer(privateKey_.constData(), privateKey_.size()),
                                     asio::ssl::context::pem);
            return context;
        });
        server_.set_open_handler([this](websocketpp::connection_hdl handle) {
            const auto connection = server_.get_con_from_hdl(handle);
            std::lock_guard<std::mutex> lock(mutex_);
            indices_[handle] = connections_.size();
            connections_.push_back(
                {handle, QByteArray::fromStdString(connection->get_request_header("Authorization")), {}});
        });
        server_.set_message_handler([this](websocketpp::connection_hdl handle, Server::message_ptr message) {
            if (message->get_opcode() != websocketpp::frame::opcode::text) return;
            const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(message->get_payload()));
            if (!document.isObject()) return;
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = indices_.find(handle);
            if (found != indices_.end()) connections_.at(found->second).frames.append(document.object());
        });
        websocketpp::lib::error_code error;
        server_.listen(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), error);
        if (error) return false;
        port_ = server_.get_local_endpoint(error).port();
        if (error || !port_) return false;
        server_.start_accept(error);
        if (error) return false;
        worker_ = std::thread([this] {
            MaiThread::setCurrentName("mai-sync-fixture");
            server_.run();
        });
        return true;
    }
    QUrl url() const {
        return QUrl(QStringLiteral("wss://localhost:%1/v1/session-sync/ws").arg(port_));
    }
    int connectionCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return int(connections_.size());
    }
    QByteArray authorization(int connection) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return connections_.at(connection).authorization;
    }
    int operationCount(const QString& operation) const {
        std::lock_guard<std::mutex> lock(mutex_);
        int count = 0;
        for (const Connection& connection : connections_)
            for (const QJsonObject& frame : connection.frames)
                if (frame.value(QStringLiteral("op")).toString() == operation) ++count;
        return count;
    }
    QJsonObject operationFrame(const QString& operation) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const Connection& connection : connections_)
            for (const QJsonObject& frame : connection.frames)
                if (frame.value(QStringLiteral("op")).toString() == operation) return frame;
        return {};
    }
    void respond(int connection, const QString& id, int status) {
        const QJsonObject response{{QStringLiteral("type"), QStringLiteral("response")},
                                   {QStringLiteral("id"), id},
                                   {QStringLiteral("status"), status},
                                   {QStringLiteral("body"), QJsonObject{}}};
        sendJson(connection, response);
    }
    void sendCatalog(int connection) {
        sendJson(connection, {{QStringLiteral("type"), QStringLiteral("catalog")},
                              {QStringLiteral("sessions"), QJsonArray{}},
                              {QStringLiteral("devices"),
                               QJsonArray{QJsonObject{{QStringLiteral("device_slot"), QStringLiteral("ios_phone")},
                                                      {QStringLiteral("device_id"), QStringLiteral("phone-install")},
                                                      {QStringLiteral("online"), true}}}}});
    }

private:
    using Server = websocketpp::server<websocketpp::config::asio_tls>;
    struct Connection {
        websocketpp::connection_hdl handle;
        QByteArray authorization;
        QList<QJsonObject> frames;
    };
    void sendJson(int connection, const QJsonObject& message) {
        websocketpp::connection_hdl handle;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            handle = connections_.at(connection).handle;
        }
        const auto payload = QJsonDocument(message).toJson(QJsonDocument::Compact).toStdString();
        server_.get_io_service().post([this, handle, payload] {
            websocketpp::lib::error_code error;
            server_.send(handle, payload, websocketpp::frame::opcode::text, error);
        });
    }

    QByteArray certificate_;
    QByteArray privateKey_;
    Server server_;
    std::thread worker_;
    unsigned short port_ = 0;
    mutable std::mutex mutex_;
    std::map<websocketpp::connection_hdl, std::size_t, std::owner_less<websocketpp::connection_hdl>> indices_;
    std::vector<Connection> connections_;
};

}  // namespace

class DesktopSessionSyncChannelTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void connectsToLocalWssWithBearer();
    void reauthenticatesWithoutRepeatingAnInFlightCommand();
    void failedReauthReconnectsWithoutRepeatingAnInFlightCommand();

private:
    TestIdentity identity_;
};

void DesktopSessionSyncChannelTest::initTestCase() {
    identity_ = createTestIdentity();
    QVERIFY(!identity_.certificate.isEmpty());
    QVERIFY(!identity_.privateKey.isEmpty());
}

void DesktopSessionSyncChannelTest::connectsToLocalWssWithBearer() {
    LocalWssServer server(identity_.certificate, identity_.privateKey);
    QVERIFY(server.start());
    const QString token(43, QLatin1Char('A'));
    DesktopSessionSyncChannel channel(server.url(), [token] { return token; }, nullptr, identity_.certificate);
    int connected = 0;
    channel.setConnectedHandler([&] { ++connected; });
    channel.start();
    QTRY_COMPARE_WITH_TIMEOUT(connected, 1, 5000);
    QCOMPARE(server.authorization(0), QByteArray("Bearer ") + token.toUtf8());
}

void DesktopSessionSyncChannelTest::reauthenticatesWithoutRepeatingAnInFlightCommand() {
    LocalWssServer server(identity_.certificate, identity_.privateKey);
    QVERIFY(server.start());
    QString token(43, QLatin1Char('A'));
    DesktopSessionSyncChannel channel(server.url(), [&token] { return token; }, nullptr, identity_.certificate);
    int connected = 0;
    channel.setConnectedHandler([&] { ++connected; });
    channel.start();
    QTRY_COMPARE_WITH_TIMEOUT(connected, 1, 5000);
    int completions = 0;
    QString commandError;
    channel.request(QStringLiteral("session-sync/send"), "POST",
                    {{QStringLiteral("kind"), QStringLiteral("command")},
                     {QStringLiteral("command_id"), QStringLiteral("same-paid-command")}},
                    [&](const QJsonObject&, const QString& error) {
                        ++completions;
                        commandError = error;
                    });
    QTRY_COMPARE_WITH_TIMEOUT(server.operationCount(QStringLiteral("send")), 1, 5000);
    const QString commandId = server.operationFrame(QStringLiteral("send")).value(QStringLiteral("id")).toString();
    token = QString(43, QLatin1Char('B'));
    channel.refreshBearerNow();
    QTRY_COMPARE_WITH_TIMEOUT(server.operationCount(QStringLiteral("reauth")), 1, 5000);
    const QJsonObject reauth = server.operationFrame(QStringLiteral("reauth"));
    QCOMPARE(reauth.value(QStringLiteral("body")).toObject().value(QStringLiteral("access_token")).toString(), token);
    QCOMPARE(server.connectionCount(), 1);
    QCOMPARE(server.operationCount(QStringLiteral("send")), 1);
    QCOMPARE(completions, 0);
    int blocked = 0;
    QString blockedError;
    channel.request(QStringLiteral("session-sync/send"), "POST",
                    {{QStringLiteral("kind"), QStringLiteral("command")},
                     {QStringLiteral("command_id"), QStringLiteral("do-not-submit-yet")}},
                    [&](const QJsonObject&, const QString& error) {
                        ++blocked;
                        blockedError = error;
                    });
    QCOMPARE(blocked, 1);
    QVERIFY(!blockedError.isEmpty());
    QCOMPARE(server.operationCount(QStringLiteral("send")), 1);
    int catalogs = 0;
    QJsonArray devices;
    channel.setCatalogHandler([&](const QJsonArray&) { ++catalogs; });
    channel.setDeviceHandler([&](const QJsonArray& value) { devices = value; });
    server.respond(0, reauth.value(QStringLiteral("id")).toString(), 200);
    server.sendCatalog(0);
    QTRY_COMPARE_WITH_TIMEOUT(catalogs, 1, 5000);
    QCOMPARE(devices.size(), 1);
    QCOMPARE(devices.first().toObject().value(QStringLiteral("device_id")).toString(), QStringLiteral("phone-install"));
    channel.refreshBearerNow();
    QTest::qWait(100);
    QCOMPARE(server.operationCount(QStringLiteral("reauth")), 1);
    server.respond(0, commandId, 200);
    QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 5000);
    QVERIFY(commandError.isEmpty());
    QCOMPARE(server.operationCount(QStringLiteral("send")), 1);
    QCOMPARE(server.connectionCount(), 1);
}

void DesktopSessionSyncChannelTest::failedReauthReconnectsWithoutRepeatingAnInFlightCommand() {
    LocalWssServer server(identity_.certificate, identity_.privateKey);
    QVERIFY(server.start());
    QString token(43, QLatin1Char('A'));
    DesktopSessionSyncChannel channel(server.url(), [&token] { return token; }, nullptr, identity_.certificate);
    int connected = 0;
    channel.setConnectedHandler([&] { ++connected; });
    channel.start();
    QTRY_COMPARE_WITH_TIMEOUT(connected, 1, 5000);
    int completions = 0;
    QString commandError;
    channel.request(QStringLiteral("session-sync/send"), "POST",
                    {{QStringLiteral("kind"), QStringLiteral("command")},
                     {QStringLiteral("command_id"), QStringLiteral("must-not-repeat")}},
                    [&](const QJsonObject&, const QString& error) {
                        ++completions;
                        commandError = error;
                    });
    QTRY_COMPARE_WITH_TIMEOUT(server.operationCount(QStringLiteral("send")), 1, 5000);
    token = QString(43, QLatin1Char('B'));
    channel.refreshBearerNow();
    QTRY_COMPARE_WITH_TIMEOUT(server.operationCount(QStringLiteral("reauth")), 1, 5000);
    const QString reauthId = server.operationFrame(QStringLiteral("reauth")).value(QStringLiteral("id")).toString();
    server.respond(0, reauthId, 401);
    QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 5000);
    QVERIFY(!commandError.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(connected, 2, 8000);
    QCOMPARE(server.authorization(1), QByteArray("Bearer ") + token.toUtf8());
    QCOMPARE(server.operationCount(QStringLiteral("send")), 1);
    QTest::qWait(200);
    QCOMPARE(completions, 1);
    QCOMPARE(server.operationCount(QStringLiteral("send")), 1);
}

QTEST_GUILESS_MAIN(DesktopSessionSyncChannelTest)
#include "DesktopSessionSyncChannelTest.moc"
