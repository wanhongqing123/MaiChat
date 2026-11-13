#include "MaiSessionIMRouter.h"

#include <atomic>
#include <exception>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using Router = MaiSessionIMRouter;
using Status = Router::Status;
#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)

namespace {

struct Fixture {
    Router router;
    Router::AccountScope scope{123, "owner", 4, 9};
    Fixture() {
        CHECK(router.activate(scope) == Status::Ok);
        CHECK(router.bind(scope, "peer", "session", "workspace-one", true, true) == Status::Ok);
    }
    Router::Incoming message(const std::string& id, const std::string& peer = "peer") const {
        Router::Incoming input;
        input.account = scope;
        input.peerId = peer;
        input.messageId = id;
        input.senderUserId = peer;
        input.recipientUserId = scope.ownerUserId;
        const auto binding = router.bindingForPeer(scope, peer);
        if (binding) { input.expectedBindingEpoch = binding->epoch; input.expectedSessionId = binding->sessionId; }
        input.origin = Router::Origin::Human;
        input.live = true;
        input.hasContent = true;
        return input;
    }
    Router::Reservation reserve() {
        const auto result = router.reserveNext(scope, "session", "workspace-one", true);
        CHECK(result.status == Status::Ok && result.reservation);
        return *result.reservation;
    }
    Router::TurnReceipt receipt(const std::string& suffix = "one") const {
        return {"user-" + suffix, "assistant-" + suffix, "turn-" + suffix};
    }
};

void scopesAreExplicitAndStaleCallsCannotChangeNewAccount() {
    Router router;
    Router::AccountScope invalid;
    CHECK(router.activate(invalid) == Status::InvalidArgument);
    Fixture f;
    CHECK(f.router.accept(f.message("before-change")).status == Status::Ok);
    const auto old = f.reserve();
    auto other = f.scope;
    ++other.authGeneration;
    CHECK(f.router.activate(other) == Status::Ok);
    CHECK(f.router.snapshot(other).bindings.empty());
    CHECK(f.router.snapshot(other).active.empty());
    CHECK(f.router.snapshot(other).pendingCount == 0);
    CHECK(!f.router.isCurrent(old.token, f.scope, "workspace-one"));
    CHECK(f.router.logout(f.scope) == Status::StaleScope);
    CHECK(f.router.snapshot(other).status == Status::Ok);
    CHECK(f.router.bind(other, "peer", "session", "workspace-one", true, true) == Status::Ok);
    auto replay = f.message("before-change");
    replay.account = other;
    replay.expectedBindingEpoch = f.router.bindingForPeer(other, "peer")->epoch;
    replay.expectedSessionId = "session";
    CHECK(f.router.accept(replay).status == Status::Duplicate);
    CHECK(f.router.logout(other) == Status::Ok);
    CHECK(f.router.snapshot(other).status == Status::NotActive);
}

void bindingEligibilityExclusivityAndEpochs() {
    Fixture f;
    for (const auto& value : {std::string(), std::string(" ")}) {
        CHECK(f.router.bind(f.scope, value, "another", "workspace", true, true) == Status::InvalidArgument);
        CHECK(f.router.bind(f.scope, "other", value, "workspace", true, true) == Status::InvalidArgument);
        CHECK(f.router.bind(f.scope, "other", "another", value, true, true) == Status::InvalidArgument);
    }
    CHECK(f.router.bind(f.scope, "owner", "another", "workspace", true, true) == Status::InvalidArgument);
    CHECK(f.router.bind(f.scope, "other", "another", "workspace", false, true) == Status::NotEligible);
    CHECK(f.router.bind(f.scope, "other", "another", "workspace", true, false) == Status::NotEligible);
    CHECK(f.router.bind(f.scope, "other", "session", "workspace", true, true) == Status::Conflict);
    const auto before = f.router.bindingForPeer(f.scope, "peer");
    CHECK(before);
    CHECK(f.router.bind(f.scope, "peer", "session", "workspace-one", true, true) == Status::Ok);
    CHECK(f.router.bindingForPeer(f.scope, "peer")->epoch == before->epoch);
    CHECK(f.router.accept(f.message("old-binding")).status == Status::Ok);
    const auto old = f.reserve();
    CHECK(f.router.bind(f.scope, "peer", "session", "workspace-two", true, true) == Status::Ok);
    CHECK(f.router.bindingForPeer(f.scope, "peer")->epoch > before->epoch);
    CHECK(!f.router.isCurrent(old.token, f.scope, "workspace-one"));
    CHECK(f.router.snapshot(f.scope).pendingCount == 0);
}

void onlyLiveHumanC2CContentCanEnter() {
    Fixture f;
    std::vector<Router::Incoming> rejected;
    auto input = f.message("filtered");
    input.origin = Router::Origin::Unknown; rejected.push_back(input);
    input = f.message("filtered"); input.origin = Router::Origin::Machine; rejected.push_back(input);
    input = f.message("filtered"); input.live = false; rejected.push_back(input);
    input = f.message("filtered"); input.group = true; rejected.push_back(input);
    input = f.message("filtered"); input.controlFrame = true; rejected.push_back(input);
    input = f.message("filtered"); input.senderUserId = "different"; rejected.push_back(input);
    input = f.message("filtered"); input.recipientUserId = "different"; rejected.push_back(input);
    input = f.message("filtered"); input.hasContent = false; rejected.push_back(input);
    for (const auto& candidate : rejected) CHECK(f.router.accept(candidate).status == Status::NotEligible);
    input = f.message("");
    CHECK(f.router.accept(input).status == Status::InvalidArgument);
    input = f.message("filtered", "owner");
    CHECK(f.router.accept(input).status == Status::InvalidArgument);
    input = f.message("filtered", "not-bound");
    CHECK(f.router.accept(input).status == Status::NotBound);
    input = f.message("filtered"); ++input.account.controllerGeneration;
    CHECK(f.router.accept(input).status == Status::StaleScope);
    CHECK(f.router.snapshot(f.scope).pendingCount == 0);
    const auto accepted = f.router.accept(f.message("filtered"));
    CHECK(accepted.status == Status::Ok && accepted.input);
    CHECK(accepted.input->binding.workspaceKey == "workspace-one");
    CHECK(accepted.input->inputId != 0);
    CHECK(f.router.accept(f.message("filtered")).status == Status::Duplicate);
}

void pendingLimitIncludesPreparationAndOverflowDoesNotReplay() {
    Fixture f;
    for (int i = 0; i < 32; ++i)
        CHECK(f.router.accept(f.message("input-" + std::to_string(i))).status == Status::Ok);
    CHECK(f.router.snapshot(f.scope).pendingCount == 32);
    CHECK(f.router.reserveNext(f.scope, "session", "workspace-one", false).status == Status::Busy);
    const auto first = f.reserve();
    CHECK(first.input.source.messageId == "input-0");
    CHECK(f.router.snapshot(f.scope).pendingCount == 32);
    CHECK(f.router.accept(f.message("overflow")).status == Status::QueueFull);
    CHECK(f.router.releaseReservation(first.token, f.scope, "workspace-one", false) == Status::Ok);
    CHECK(f.router.snapshot(f.scope).pendingCount == 31);
    CHECK(f.router.accept(f.message("overflow")).status == Status::Duplicate);
    CHECK(f.reserve().input.source.messageId == "input-1");
}

void busyRetryKeepsExactInputAtFrontButRenewsReservation() {
    Fixture f;
    const auto admitted = f.router.accept(f.message("first"));
    CHECK(f.router.accept(f.message("second")).status == Status::Ok);
    const auto first = f.reserve();
    CHECK(f.router.releaseReservation(first.token, f.scope, "workspace-one", true) == Status::Ok);
    const auto retried = f.reserve();
    CHECK(retried.token != first.token);
    CHECK(retried.input.inputId == admitted.input->inputId);
    CHECK(retried.input.source.messageId == "first");
    CHECK(!f.router.isCurrent(first.token, f.scope, "workspace-one"));
    CHECK(f.router.snapshot(f.scope).pendingCount == 2);
    CHECK(f.router.releaseReservation(retried.token, f.scope, "workspace-one", false) == Status::Ok);
    CHECK(f.reserve().input.source.messageId == "second");
}

void exactReceiptsAndUniqueDispatchAcksHoldFifo() {
    Fixture f;
    CHECK(f.router.accept(f.message("first")).status == Status::Ok);
    CHECK(f.router.accept(f.message("second")).status == Status::Ok);
    const auto reservation = f.reserve();
    auto receipt = f.receipt();
    auto invalid = receipt; invalid.turnId.clear();
    CHECK(f.router.bindReceipt(reservation.token, invalid, f.scope, "workspace-one") == Status::InvalidReceipt);
    invalid = receipt; invalid.assistantMessageId = invalid.userMessageId;
    CHECK(f.router.bindReceipt(reservation.token, invalid, f.scope, "workspace-one") == Status::InvalidReceipt);
    CHECK(f.router.bindReceipt(reservation.token, receipt, f.scope, "workspace-one") == Status::Ok);
    CHECK(f.router.snapshot(f.scope).pendingCount == 1);
    CHECK(f.router.snapshot(f.scope).active.front().phase == Router::Phase::Running);
    CHECK(!f.router.isOutputCurrent(reservation.token, receipt, f.scope, "workspace-one"));
    invalid = receipt; invalid.assistantMessageId = "unrelated-newest-assistant";
    CHECK(f.router.beginOutput(reservation.token, invalid, f.scope, "workspace-one", 2) == Status::InvalidReceipt);
    CHECK(f.router.beginOutput(reservation.token, receipt, f.scope, "workspace-one", 0) == Status::InvalidArgument);
    CHECK(f.router.beginOutput(reservation.token, receipt, f.scope, "workspace-one", 2) == Status::Ok);
    CHECK(f.router.beginOutput(reservation.token, receipt, f.scope, "workspace-one", 2) == Status::Busy);
    CHECK(f.router.isOutputCurrent(reservation.token, receipt, f.scope, "workspace-one"));
    CHECK(!f.router.isOutputCurrent(reservation.token, invalid, f.scope, "workspace-one"));
    CHECK(f.router.acknowledgeDispatch(reservation.token, receipt, f.scope, "workspace-one", 2) == Status::InvalidArgument);
    CHECK(f.router.acknowledgeDispatch(reservation.token, receipt, f.scope, "workspace-one", 0) == Status::Ok);
    CHECK(f.router.acknowledgeDispatch(reservation.token, receipt, f.scope, "workspace-one", 0) == Status::Duplicate);
    CHECK(f.router.snapshot(f.scope).active.front().acknowledgedDispatches == 1);
    CHECK(f.router.reserveNext(f.scope, "session", "workspace-one", true).status == Status::Busy);
    CHECK(f.router.acknowledgeDispatch(reservation.token, receipt, f.scope, "workspace-one", 1) == Status::Ok);
    CHECK(!f.router.isOutputCurrent(reservation.token, receipt, f.scope, "workspace-one"));
    CHECK(f.router.acknowledgeDispatch(reservation.token, receipt, f.scope, "workspace-one", 1) == Status::InvalidToken);
    CHECK(f.reserve().input.source.messageId == "second");
}

void failedCanonicalTurnReleasesWithoutOutput() {
    Fixture f;
    CHECK(f.router.accept(f.message("failed")).status == Status::Ok);
    const auto reservation = f.reserve();
    const auto receipt = f.receipt();
    CHECK(f.router.finishWithoutOutput(reservation.token, receipt, f.scope, "workspace-one") == Status::InvalidPhase);
    CHECK(f.router.bindReceipt(reservation.token, receipt, f.scope, "workspace-one") == Status::Ok);
    CHECK(f.router.finishWithoutOutput(reservation.token, f.receipt("wrong"), f.scope, "workspace-one") == Status::InvalidReceipt);
    CHECK(f.router.finishWithoutOutput(reservation.token, receipt, f.scope, "workspace-one") == Status::Ok);
    CHECK(f.router.snapshot(f.scope).active.empty());
    CHECK(!f.router.reserveNext(f.scope, "session", "workspace-one", true).reservation);
}

void everyRevocationPhaseRemovesAuthorityAndKeepsOtherSession() {
    for (int phase = 0; phase < 3; ++phase) {
        Fixture f;
        CHECK(f.router.bind(f.scope, "other", "other-session", "other-workspace", true, true) == Status::Ok);
        CHECK(f.router.accept(f.message("other-input", "other")).status == Status::Ok);
        CHECK(f.router.accept(f.message("current")).status == Status::Ok);
        CHECK(f.router.accept(f.message("queued")).status == Status::Ok);
        const auto reservation = f.reserve();
        const auto receipt = f.receipt();
        if (phase > 0) CHECK(f.router.bindReceipt(reservation.token, receipt, f.scope, "workspace-one") == Status::Ok);
        if (phase > 1) CHECK(f.router.beginOutput(reservation.token, receipt, f.scope, "workspace-one", 1) == Status::Ok);
        CHECK(!f.router.isCurrent(reservation.token, f.scope, "changed-workspace"));
        CHECK(f.router.revokeSession(f.scope, "session") == Status::Ok);
        CHECK(!f.router.bindingForPeer(f.scope, "peer"));
        CHECK(!f.router.isCurrent(reservation.token, f.scope, "workspace-one"));
        CHECK(!f.router.isOutputCurrent(reservation.token, receipt, f.scope, "workspace-one"));
        const auto snapshot = f.router.snapshot(f.scope);
        CHECK(snapshot.pendingCount == 1 && snapshot.bindings.size() == 1 && snapshot.active.empty());
        CHECK(f.router.reserveNext(f.scope, "other-session", "other-workspace", true).reservation);
    }
}

void rememberedInputsStayBoundedAndSeparatePeersAndOwners() {
    Fixture f;
    for (int index = 0; index < 513; ++index) {
        CHECK(f.router.accept(f.message("remember-" + std::to_string(index))).status == Status::Ok);
        const auto reservation = f.reserve();
        CHECK(f.router.releaseReservation(reservation.token, f.scope, "workspace-one", false) == Status::Ok);
    }
    CHECK(f.router.snapshot(f.scope).rememberedInputs == 512);
    CHECK(f.router.accept(f.message("remember-512")).status == Status::Duplicate);
    CHECK(f.router.accept(f.message("remember-0")).status == Status::Ok);
    CHECK(f.router.bind(f.scope, "other", "other-session", "other-workspace", true, true) == Status::Ok);
    CHECK(f.router.accept(f.message("remember-0", "other")).status == Status::Ok);
    auto next = f.scope; next.ownerUserId = "other-owner";
    CHECK(f.router.activate(next) == Status::Ok);
    CHECK(f.router.bind(next, "peer", "session", "workspace-one", true, true) == Status::Ok);
    auto input = f.message("remember-0"); input.account = next; input.recipientUserId = next.ownerUserId;
    input.expectedBindingEpoch = f.router.bindingForPeer(next, "peer")->epoch;
    input.expectedSessionId = "session";
    CHECK(f.router.accept(input).status == Status::Ok);
}

void concurrentAdmissionsRespectOneGlobalPendingBound() {
    Fixture f;
    std::atomic<int> accepted{0};
    std::atomic<int> full{0};
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 8; ++thread) threads.emplace_back([&, thread] {
        for (int index = 0; index < 8; ++index) {
            const auto status = f.router.accept(f.message("concurrent-" + std::to_string(thread * 8 + index))).status;
            if (status == Status::Ok) ++accepted;
            else if (status == Status::QueueFull) ++full;
        }
    });
    for (auto& thread : threads) thread.join();
    CHECK(accepted == 32 && full == 32);
    const auto snapshot = f.router.snapshot(f.scope);
    CHECK(snapshot.pendingCount == 32 && snapshot.pending.size() == 32 && snapshot.rememberedInputs == 64);
    std::uint64_t previous = 0;
    for (const auto& input : snapshot.pending) { CHECK(input.inputId > previous); previous = input.inputId; }
}

void lateImportedInputCannotAdoptAReplacementBinding() {
    Fixture f;
    const auto ingress = f.message("importing-before-rebind");
    CHECK(f.router.bind(f.scope, "peer", "replacement", "workspace-two", true, true) == Status::Ok);
    CHECK(f.router.accept(ingress).status == Status::StaleScope);
    CHECK(f.router.snapshot(f.scope).pendingCount == 0);
    auto fresh = f.message("fresh-after-rebind");
    CHECK(f.router.accept(fresh).status == Status::Ok);
    const auto importingSameSession = f.message("importing-before-workspace-change");
    CHECK(f.router.bind(f.scope, "peer", "replacement", "workspace-three", true, true) == Status::Ok);
    CHECK(f.router.accept(importingSameSession).status == Status::StaleScope);
    CHECK(f.router.snapshot(f.scope).pendingCount == 0);
}

void missingPayloadHoldsOriginalFifoUntilExactInputIsDiscarded() {
    Fixture f;
    const auto media = f.router.accept(f.message("media-not-downloaded"));
    const auto text = f.router.accept(f.message("later-text-ready"));
    CHECK(media.input && text.input);
    const auto held = f.reserve();
    CHECK(held.input.inputId == media.input->inputId);
    CHECK(f.router.reserveNext(f.scope, "session", "workspace-one", true).status == Status::Busy);
    CHECK(f.router.discardInput(f.scope, media.input->inputId) == Status::Ok);
    const auto next = f.reserve();
    CHECK(next.input.inputId == text.input->inputId);
    CHECK(!f.router.isCurrent(held.token, f.scope, "workspace-one"));
    CHECK(f.router.bindReceipt(next.token, f.receipt(), f.scope, "workspace-one") == Status::Ok);
    CHECK(f.router.discardInput(f.scope, text.input->inputId) == Status::InvalidPhase);
    CHECK(f.router.beginOutput(next.token, f.receipt(), f.scope, "workspace-one", 1) == Status::Ok);
    CHECK(f.router.discardInput(f.scope, text.input->inputId) == Status::InvalidPhase);
}

void revocationWinsOverLateDispatchCallbacks() {
    for (int repeat = 0; repeat < 40; ++repeat) {
        Fixture f;
        CHECK(f.router.accept(f.message("active")).status == Status::Ok);
        CHECK(f.router.accept(f.message("next")).status == Status::Ok);
        const auto reservation = f.reserve();
        const auto receipt = f.receipt();
        CHECK(f.router.bindReceipt(reservation.token, receipt, f.scope, "workspace-one") == Status::Ok);
        CHECK(f.router.beginOutput(reservation.token, receipt, f.scope, "workspace-one", 1) == Status::Ok);
        std::thread ack([&] { f.router.acknowledgeDispatch(reservation.token, receipt, f.scope, "workspace-one", 0); });
        std::thread revoke([&] { f.router.unbind(f.scope, "peer"); });
        ack.join(); revoke.join();
        CHECK(!f.router.isOutputCurrent(reservation.token, receipt, f.scope, "workspace-one"));
        const auto snapshot = f.router.snapshot(f.scope);
        CHECK(snapshot.bindings.empty() && snapshot.pendingCount == 0 && snapshot.active.empty());
    }
}

void historyBarrierAdmitsButNeverRunsBeforeTheMatchingCommit() {
    Fixture f;
    const auto epoch = f.router.bindingForPeer(f.scope, "peer")->epoch;
    CHECK(f.router.beginHistory(f.scope, "peer", epoch) == Status::Ok);
    CHECK(f.router.snapshot(f.scope).bindings.front().historyPending);
    CHECK(f.router.accept(f.message("live-during-history")).status == Status::Ok);
    CHECK(f.router.snapshot(f.scope).pendingCount == 1);
    CHECK(f.router.reserveNext(f.scope, "session", "workspace-one", true).status == Status::Busy);
    CHECK(f.router.completeHistory(f.scope, "peer", epoch + 1) == Status::StaleScope);
    CHECK(f.router.reserveNext(f.scope, "session", "workspace-one", true).status == Status::Busy);
    CHECK(f.router.completeHistory(f.scope, "peer", epoch) == Status::Ok);
    CHECK(!f.router.snapshot(f.scope).bindings.front().historyPending);
    CHECK(f.reserve().input.source.messageId == "live-during-history");
    CHECK(f.router.beginHistory(f.scope, "peer", epoch) == Status::Busy);
    CHECK(f.router.unbind(f.scope, "peer") == Status::Ok);
    CHECK(f.router.completeHistory(f.scope, "peer", epoch) == Status::NotBound);
    CHECK(f.router.snapshot(f.scope).pendingCount == 0);
}

void historyOverlapWaitsForActualPersistenceAndKeepsBenignReplay() {
    Fixture f;
    const auto epoch = f.router.bindingForPeer(f.scope, "peer")->epoch;
    CHECK(f.router.beginHistory(f.scope, "peer", epoch) == Status::Ok);
    auto replay = f.message("replayed-remote");
    replay.requirePersistence = true;
    const auto old = f.router.accept(replay);
    CHECK(old.status == Status::Ok && old.input && !old.input->persistenceKnown);
    CHECK(f.router.historyMatch(f.scope, "peer", epoch, "existing-local", "replayed-remote", "peer", "owner") ==
          Router::HistoryMatch::AwaitingPersistence);
    CHECK(f.router.confirmPersisted(f.scope, old.input->inputId, false, "existing-local", "replayed-remote") == Status::Ok);
    CHECK(f.router.snapshot(f.scope).pendingCount == 0);
    CHECK(f.router.historyMatch(f.scope, "peer", epoch, "existing-local", "replayed-remote", "peer", "owner") ==
          Router::HistoryMatch::None);
    auto fresh = f.message("fresh-remote");
    fresh.requirePersistence = true;
    const auto live = f.router.accept(fresh);
    CHECK(live.status == Status::Ok && live.input);
    CHECK(f.router.confirmPersisted(f.scope, live.input->inputId, true, "fresh-local", "fresh-remote") == Status::Ok);
    CHECK(f.router.confirmPersisted(f.scope, live.input->inputId, true, "fresh-local", "fresh-remote") == Status::Duplicate);
    CHECK(f.router.historyMatch(f.scope, "peer", epoch, "fresh-local", "fresh-remote", "peer", "owner") ==
          Router::HistoryMatch::LiveInput);
    CHECK(f.router.historyMatch(f.scope, "peer", epoch, "fresh-local", "fresh-remote", "other", "owner") ==
          Router::HistoryMatch::None);
    CHECK(f.router.historyMatch(f.scope, "peer", epoch + 1, "fresh-local", "fresh-remote", "peer", "owner") ==
          Router::HistoryMatch::Stale);
    CHECK(f.router.completeHistory(f.scope, "peer", epoch) == Status::Ok);
    const auto next = f.reserve();
    CHECK(next.input.persistenceKnown && next.input.persistedRowId == "fresh-local");
}

}  // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"scope boundaries", scopesAreExplicitAndStaleCallsCannotChangeNewAccount},
        {"binding eligibility and epochs", bindingEligibilityExclusivityAndEpochs},
        {"live Human C2C filter", onlyLiveHumanC2CContentCanEnter},
        {"pending bound and overflow dedupe", pendingLimitIncludesPreparationAndOverflowDoesNotReplay},
        {"Busy FIFO retry", busyRetryKeepsExactInputAtFrontButRenewsReservation},
        {"exact receipts and dispatch FIFO", exactReceiptsAndUniqueDispatchAcksHoldFifo},
        {"failed canonical turn", failedCanonicalTurnReleasesWithoutOutput},
        {"revocation in every phase", everyRevocationPhaseRemovesAuthorityAndKeepsOtherSession},
        {"bounded account-aware dedupe", rememberedInputsStayBoundedAndSeparatePeersAndOwners},
        {"concurrent admissions", concurrentAdmissionsRespectOneGlobalPendingBound},
        {"late imported input binding epoch", lateImportedInputCannotAdoptAReplacementBinding},
        {"admission before media readiness FIFO", missingPayloadHoldsOriginalFifoUntilExactInputIsDiscarded},
        {"late dispatch revocation", revocationWinsOverLateDispatchCallbacks},
        {"history commit live barrier", historyBarrierAdmitsButNeverRunsBeforeTheMatchingCommit},
        {"history persistence overlap", historyOverlapWaitsForActualPersistenceAndKeepsBenignReplay}};
    int failed = 0;
    for (const auto& test : tests) {
        try { test.second(); std::cout << "PASS " << test.first << '\n'; }
        catch (const std::exception& error) { ++failed; std::cout << "FAIL " << test.first << ": " << error.what() << '\n'; }
    }
    std::cout << "TOTAL " << tests.size() - failed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
