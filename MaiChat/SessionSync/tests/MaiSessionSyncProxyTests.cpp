// Offline proxy URI contract. This includes the private implementation so that
// credentials do not require a new public API or a networked test hook.
#include "../MaiSessionSyncSocket.cpp"

#include <iostream>

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        if (!ok) {
            ++failures;
            std::cerr << "FAIL: " << name << '\n';
        }
    };
    ProxyAuthentication direct;
    check(parseProxyAuthentication("", direct) && direct.url.empty() && !direct.authenticated,
          "direct remains explicit");
    ProxyAuthentication plain;
    check(parseProxyAuthentication("http://localhost:8888/", plain) && plain.url == "http://localhost:8888/" &&
              !plain.authenticated,
          "plain HTTP CONNECT proxy");
    ProxyAuthentication authentication;
    check(parseProxyAuthentication("http://user%40name:p%3A%25word@[::1]:8888/", authentication) &&
              authentication.url == "http://[::1]:8888/" && authentication.username == "user@name" &&
              authentication.password == "p:%word" && authentication.authenticated,
          "userinfo is decoded and removed from URI");
    ProxyAuthentication emptyPassword;
    check(parseProxyAuthentication("http://fixture:@localhost:8888", emptyPassword) &&
              emptyPassword.username == "fixture" && emptyPassword.password.empty() && emptyPassword.authenticated,
          "empty password is preserved");
    for (const auto* invalid : {"socks5h://localhost:8888", "https://localhost:8888", "http://localhost:0/",
                                "http://localhost:99999/", "http://user%3Aname:secret@localhost:8888/",
                                "http://user%0Dname:secret@localhost:8888/", "http://localhost:8888/path",
                                "http://localhost:8888/?x=y", "http://localhost:8888/#fragment", "http://:8888/"}) {
        ProxyAuthentication rejected;
        check(!parseProxyAuthentication(invalid, rejected), "invalid proxy is rejected");
    }
    if (failures) return 1;
    std::cout << "session sync proxy tests passed\n";
    return 0;
}
