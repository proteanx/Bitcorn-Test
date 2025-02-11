// Copyright (c) 2018-2019 The Dash Core developers
// Copyright (c) 2019 The BitCorn Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/quorums_dkgsession.h>

#include <llmq/quorums_commitment.h>
#include <llmq/quorums_debug.h>
#include <llmq/quorums_dkgsessionmgr.h>
#include <llmq/quorums_utils.h>

#include <special/specialtx.h>

#include <masternodes/activemasternode.h>
#include <chainparams.h>
#include <init.h>
#include <net.h>
#include <netmessagemaker.h>
#include <univalue.h>
#include <validation.h>

#include <cxxtimer.hpp>

namespace llmq
{

// Supported error types:
// - contribution-omit
// - contribution-lie
// - complain-lie
// - justify-lie
// - justify-omit
// - commit-omit
// - commit-lie

static CCriticalSection cs_simDkgError;
static std::map<std::string, double> simDkgErrorMap;

void SetSimulatedDKGErrorRate(const std::string& type, double rate)
{
    LOCK(cs_simDkgError);
    simDkgErrorMap[type] = rate;
}

static double GetSimulatedErrorRate(const std::string& type)
{
    LOCK(cs_simDkgError);
    auto it = simDkgErrorMap.find(type);
    if (it != simDkgErrorMap.end()) {
        return it->second;
    }
    return 0;
}

static bool ShouldSimulateError(const std::string& type)
{
    double rate = GetSimulatedErrorRate(type);
    return GetRandBool(rate);
}

CDKGComplaint::CDKGComplaint(const Consensus::LLMQParams& params) :
    badMembers((size_t)params.size), complainForMembers((size_t)params.size)
{
}

CDKGPrematureCommitment::CDKGPrematureCommitment(const Consensus::LLMQParams& params) :
    validMembers((size_t)params.size)
{
}

CDKGMember::CDKGMember(CDeterministicMNCPtr _dmn, size_t _idx) :
    dmn(_dmn),
    idx(_idx),
    id(CBLSId::FromHash(_dmn->proTxHash))
{

}

bool CDKGSession::Init(const CBlockIndex* _pindexQuorum, const std::vector<CDeterministicMNCPtr>& mns, const uint256& _myProTxHash)
{
    if (mns.size() < params.minSize) {
        return false;
    }

    pindexQuorum = _pindexQuorum;

    members.resize(mns.size());
    memberIds.resize(members.size());
    receivedVvecs.resize(members.size());
    receivedSkContributions.resize(members.size());

    for (size_t i = 0; i < mns.size(); i++) {
        members[i] = std::unique_ptr<CDKGMember>(new CDKGMember(mns[i], i));
        membersMap.emplace(members[i]->dmn->proTxHash, i);
        memberIds[i] = members[i]->id;
    }

    if (!_myProTxHash.IsNull()) {
        for (size_t i = 0; i < members.size(); i++) {
            auto& m = members[i];
            if (m->dmn->proTxHash == _myProTxHash) {
                myIdx = i;
                myProTxHash = _myProTxHash;
                myId = m->id;
                break;
            }
        }
    }

    if (!myProTxHash.IsNull()) {
        quorumDKGDebugManager->InitLocalSessionStatus(params.type, pindexQuorum->GetBlockHash(), pindexQuorum->nHeight);
    }

    if (myProTxHash.IsNull())
        LogPrint(BCLog::LLMQDKG, "CDKGSession initialized as observer. mns=%d\n", mns.size());
    else
        LogPrint(BCLog::LLMQDKG, "CDKGSession initialized as member. mns=%d\n", mns.size());

    return true;
}

void CDKGSession::Contribute(CDKGPendingMessages& pendingMessages)
{
    if (!AreWeMember())
        return;

    cxxtimer::Timer t1(true);
    LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: generating contributions\n", __func__);
    if (!blsWorker.GenerateContributions(params.threshold, memberIds, vvecContribution, skContributions)) {
        // this should never happen actually
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: GenerateContributions failed\n", __func__);
        return;
    }
    LogPrint(BCLog::BENCHMARK, "CDKGSession::%s: generated contributions. time=%d\n", __func__, t1.count());

    SendContributions(pendingMessages);
}

void CDKGSession::SendContributions(CDKGPendingMessages& pendingMessages)
{
    assert(AreWeMember());

    LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: sending contributions\n", __func__);

    if (ShouldSimulateError("contribution-omit")) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: omitting\n", __func__);
        return;
    }

    CDKGContribution qc;
    qc.llmqType = (uint8_t)params.type;
    qc.quorumHash = pindexQuorum->GetBlockHash();
    qc.proTxHash = myProTxHash;
    qc.vvec = vvecContribution;

    cxxtimer::Timer t1(true);
    qc.contributions = std::make_shared<CBLSIESMultiRecipientObjects<CBLSSecretKey>>();
    qc.contributions->InitEncrypt(members.size());

    for (size_t i = 0; i < members.size(); i++) {
        auto& m = members[i];
        CBLSSecretKey skContrib = skContributions[i];

        if (i != myIdx && ShouldSimulateError("contribution-lie")) {
            LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: lying for %s\n", __func__, m->dmn->proTxHash.ToString());
            skContrib.MakeNewKey();
        }

        if (!qc.contributions->Encrypt(i, m->dmn->pdmnState->pubKeyOperator.Get(), skContrib, PROTOCOL_VERSION)) {
            LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: failed to encrypt contribution for %s\n", __func__, m->dmn->proTxHash.ToString());
            return;
        }
    }

    LogPrint(BCLog::BENCHMARK, "CDKGSession::%s: encrypted contributions. time=%d\n", __func__, t1.count());

    qc.sig = activeMasternodeInfo.blsKeyOperator->Sign(qc.GetSignHash());

    quorumDKGDebugManager->UpdateLocalSessionStatus(params.type, [&](CDKGDebugSessionStatus& status) {
        status.sentContributions = true;
        return true;
    });

    pendingMessages.PushPendingMessage(-1, qc);
}

// only performs cheap verifications, but not the signature of the message. this is checked with batched verification
bool CDKGSession::PreVerifyMessage(const uint256& hash, const CDKGContribution& qc, bool& retBan) const
{
    cxxtimer::Timer t1(true);

    retBan = false;

    if (qc.quorumHash != pindexQuorum->GetBlockHash()) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: contribution for wrong quorum, rejecting\n", __func__);
        return false;
    }

    auto member = GetMember(qc.proTxHash);
    if (!member) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: contributor not a member of this quorum, rejecting contribution\n", __func__);
        retBan = true;
        return false;
    }

    if (qc.contributions->blobs.size() != members.size()) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: invalid contributions count\n", __func__);
        retBan = true;
        return false;
    }
    if (qc.vvec->size() != params.threshold) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: invalid verification vector length\n", __func__);
        retBan = true;
        return false;
    }

    if (!blsWorker.VerifyVerificationVector(*qc.vvec)) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: invalid verification vector\n", __func__);
        retBan = true;
        return false;
    }

    if (member->contributions.size() >= 2) {
        // don't do any further processing if we got more than 1 valid contributions already
        // this is a DoS protection against members sending multiple contributions with valid signatures to us
        // we must bail out before any expensive BLS verification happens
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: dropping contribution from %s as we already got %d contributions\n",
            __func__, member->dmn->proTxHash.ToString(), member->contributions.size());
        return false;
    }

    return true;
}

void CDKGSession::ReceiveMessage(const uint256& hash, const CDKGContribution& qc, bool& retBan)
{
    retBan = false;

    auto member = GetMember(qc.proTxHash);

    cxxtimer::Timer t1(true);
    LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: received contribution from %s\n", __func__, qc.proTxHash.ToString());

    {
        // relay, no matter if further verification fails
        // This ensures the whole quorum sees the bad behavior
        LOCK(invCs);

        if (member->contributions.size() >= 2) {
            // only relay up to 2 contributions, that's enough to let the other members know about his bad behavior
            return;
        }

        contributions.emplace(hash, qc);
        member->contributions.emplace(hash);

        CInv inv(MSG_QUORUM_CONTRIB, hash);
        invSet.emplace(inv);
        RelayInvToParticipants(inv);

        quorumDKGDebugManager->UpdateLocalMemberStatus(params.type, member->idx, [&](CDKGDebugMemberStatus& status) {
            status.receivedContribution = true;
            return true;
        });

        if (member->contributions.size() > 1) {
            // don't do any further processing if we got more than 1 contribution. we already relayed it,
            // so others know about his bad behavior
            MarkBadMember(member->idx);
            LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: %s did send multiple contributions\n", __func__, member->dmn->proTxHash.ToString());
            return;
        }
    }

    receivedVvecs[member->idx] = qc.vvec;

    int receivedCount = 0;
    for (const auto& m : members) {
        if (!m->contributions.empty()) {
            receivedCount++;
        }
    }

    LogPrint(BCLog::BENCHMARK, "CDKGSession::%s: received and relayed contribution. received=%d/%d, time=%d\n", __func__, receivedCount, members.size(), t1.count());

    cxxtimer::Timer t2(true);

    if (!AreWeMember()) {
        // can't further validate
        return;
    }

    dkgManager.WriteVerifiedVvecContribution(params.type, pindexQuorum, qc.proTxHash, qc.vvec);

    bool complain = false;
    CBLSSecretKey skContribution;
    if (!qc.contributions->Decrypt(myIdx, *activeMasternodeInfo.blsKeyOperator, skContribution, PROTOCOL_VERSION)) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: contribution from %s could not be decrypted\n", __func__, member->dmn->proTxHash.ToString());
        complain = true;
    } else if (member->idx != myIdx && ShouldSimulateError("complain-lie")) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: lying/complaining for %s\n", __func__, member->dmn->proTxHash.ToString());
        complain = true;
    }

    if (complain) {
        member->weComplain = true;
        quorumDKGDebugManager->UpdateLocalMemberStatus(params.type, member->idx, [&](CDKGDebugMemberStatus& status) {
            status.weComplain = true;
            return true;
        });
        return;
    }

    LogPrint(BCLog::BENCHMARK, "CDKGSession::%s: decrypted our contribution share. time=%d\n", __func__, t2.count());

    bool verifyPending = false;
    receivedSkContributions[member->idx] = skContribution;
    pendingContributionVerifications.emplace_back(member->idx);
    if (pendingContributionVerifications.size() >= 32) {
        verifyPending = true;
    }

    if (verifyPending) {
        VerifyPendingContributions();
    }
}

// Verifies all pending secret key contributions in one batch
// This is done by aggregating the verification vectors belonging to the secret key contributions
// The resulting aggregated vvec is then used to recover a public key share
// The public key share must match the public key belonging to the aggregated secret key contributions
// See CBLSWorker::VerifyContributionShares for more details.
void CDKGSession::VerifyPendingContributions()
{
    cxxtimer::Timer t1(true);

    std::vector<size_t> pend = std::move(pendingContributionVerifications);
    if (pend.empty()) {
        return;
    }

    std::vector<size_t> memberIndexes;
    std::vector<BLSVerificationVectorPtr> vvecs;
    BLSSecretKeyVector skContributions;

    for (const auto& idx : pend) {
        auto& m = members[idx];
        if (m->bad || m->weComplain) {
            continue;
        }
        memberIndexes.emplace_back(idx);
        vvecs.emplace_back(receivedVvecs[idx]);
        skContributions.emplace_back(receivedSkContributions[idx]);
    }

    auto result = blsWorker.VerifyContributionShares(myId, vvecs, skContributions);
    if (result.size() != memberIndexes.size()) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: VerifyContributionShares returned result of size %d but size %d was expected, something is wrong\n",
            __func__, result.size(), memberIndexes.size());
        return;
    }

    for (size_t i = 0; i < memberIndexes.size(); i++) {
        if (!result[i]) {
            auto& m = members[memberIndexes[i]];
            LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: invalid contribution from %s. will complain later\n",
                __func__, m->dmn->proTxHash.ToString());
            m->weComplain = true;
            quorumDKGDebugManager->UpdateLocalMemberStatus(params.type, m->idx, [&](CDKGDebugMemberStatus& status) {
                status.weComplain = true;
                return true;
            });
        } else {
            size_t memberIdx = memberIndexes[i];
            dkgManager.WriteVerifiedSkContribution(params.type, pindexQuorum, members[memberIdx]->dmn->proTxHash, skContributions[i]);
        }
    }

    LogPrint(BCLog::BENCHMARK, "CDKGSession::%s: verified %d pending contributions. time=%d\n",
        __func__, pend.size(), t1.count());
}

void CDKGSession::VerifyAndComplain(CDKGPendingMessages& pendingMessages)
{
    if (!AreWeMember()) {
        return;
    }

    VerifyPendingContributions();

    // we check all members if they sent us their contributions
    // we consider members as bad if they missed to send anything or if they sent multiple
    // in both cases we won't give him a second chance as he is either down, buggy or an adversary
    // we assume that such a participant will be marked as bad by the whole network in most cases,
    // as propagation will ensure that all nodes see the same vvecs/contributions. In case nodes come to
    // different conclusions, the aggregation phase will handle this (most voted quorum key wins)

    cxxtimer::Timer t1(true);

    for (const auto& m : members) {
        if (m->bad) {
            continue;
        }
        if (m->contributions.empty()) {
            LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: %s did not send any contribution\n",
                __func__, m->dmn->proTxHash.ToString());
            MarkBadMember(m->idx);
            continue;
        }
    }

    LogPrint(BCLog::BENCHMARK, "CDKGSession::%s: verified contributions. time=%d\n",
        __func__, t1.count());

    SendComplaint(pendingMessages);
}

void CDKGSession::SendComplaint(CDKGPendingMessages& pendingMessages)
{
    assert(AreWeMember());

    CDKGComplaint qc(params);
    qc.llmqType = (uint8_t)params.type;
    qc.quorumHash = pindexQuorum->GetBlockHash();
    qc.proTxHash = myProTxHash;

    int badCount = 0;
    int complaintCount = 0;
    for (size_t i = 0; i < members.size(); i++) {
        auto& m = members[i];
        if (m->bad) {
            qc.badMembers[i] = true;
            badCount++;
        } else if (m->weComplain) {
            qc.complainForMembers[i] = true;
            complaintCount++;
        }
    }

    if (badCount == 0 && complaintCount == 0) {
        return;
    }

    LogPrint(BCLog::BENCHMARK, "CDKGSession::%s: sending complaint. badCount=%d, complaintCount=%d\n",
        __func__, badCount, complaintCount);

    qc.sig = activeMasternodeInfo.blsKeyOperator->Sign(qc.GetSignHash());

    quorumDKGDebugManager->UpdateLocalSessionStatus(params.type, [&](CDKGDebugSessionStatus& status) {
        status.sentComplaint = true;
        return true;
    });

    pendingMessages.PushPendingMessage(-1, qc);
}

// only performs cheap verifications, but not the signature of the message. this is checked with batched verification
bool CDKGSession::PreVerifyMessage(const uint256& hash, const CDKGComplaint& qc, bool& retBan) const
{
    retBan = false;

    if (qc.quorumHash != pindexQuorum->GetBlockHash()) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: complaint for wrong quorum, rejecting\n", __func__);
        return false;
    }

    auto member = GetMember(qc.proTxHash);
    if (!member) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: complainer not a member of this quorum, rejecting complaint\n", __func__);
        retBan = true;
        return false;
    }

    if (qc.badMembers.size() != (size_t)params.size) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: invalid badMembers bitset size\n", __func__);
        retBan = true;
        return false;
    }

    if (qc.complainForMembers.size() != (size_t)params.size) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: invalid complainForMembers bitset size\n", __func__);
        retBan = true;
        return false;
    }

    if (member->complaints.size() >= 2) {
        // don't do any further processing if we got more than 1 valid complaints already
        // this is a DoS protection against members sending multiple complaints with valid signatures to us
        // we must bail out before any expensive BLS verification happens
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: dropping complaint from %s as we already got %d complaints\n",
            __func__, member->dmn->proTxHash.ToString(), member->complaints.size());
        return false;
    }

    return true;
}

void CDKGSession::ReceiveMessage(const uint256& hash, const CDKGComplaint& qc, bool& retBan)
{
    retBan = false;

    LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: received complaint from %s\n", __func__, qc.proTxHash.ToString());

    auto member = GetMember(qc.proTxHash);

    {
        LOCK(invCs);

        if (member->complaints.size() >= 2) {
            // only relay up to 2 complaints, that's enough to let the other members know about his bad behavior
            return;
        }

        complaints.emplace(hash, qc);
        member->complaints.emplace(hash);

        CInv inv(MSG_QUORUM_COMPLAINT, hash);
        invSet.emplace(inv);
        RelayInvToParticipants(inv);

        quorumDKGDebugManager->UpdateLocalMemberStatus(params.type, member->idx, [&](CDKGDebugMemberStatus& status) {
            status.receivedComplaint = true;
            return true;
        });

        if (member->complaints.size() > 1) {
            // don't do any further processing if we got more than 1 complaint. we already relayed it,
            // so others know about his bad behavior
            MarkBadMember(member->idx);
            LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: %s did send multiple complaints\n", __func__, member->dmn->proTxHash.ToString());
            return;
        }
    }

    int receivedCount = 0;
    for (size_t i = 0; i < members.size(); i++) {
        auto& m = members[i];
        if (qc.badMembers[i]) {
            LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: %s voted for %s to be bad\n",
                __func__, member->dmn->proTxHash.ToString(), m->dmn->proTxHash.ToString());
            m->badMemberVotes.emplace(qc.proTxHash);
            if (AreWeMember() && i == myIdx) {
                LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: %s voted for us to be bad\n",
                    __func__, member->dmn->proTxHash.ToString());
            }
        }
        if (qc.complainForMembers[i]) {
            m->complaintsFromOthers.emplace(qc.proTxHash);
            m->someoneComplain = true;
            quorumDKGDebugManager->UpdateLocalMemberStatus(params.type, m->idx, [&](CDKGDebugMemberStatus& status) {
                return status.complaintsFromMembers.emplace(member->idx).second;
            });
            if (AreWeMember() && i == myIdx) {
                LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: %s complained about us\n",
                    __func__, member->dmn->proTxHash.ToString());
            }
        }
        if (!m->complaints.empty()) {
            receivedCount++;
        }
    }

    LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: received and relayed complaint. received=%d\n",
        __func__, receivedCount);
}

void CDKGSession::VerifyAndJustify(CDKGPendingMessages& pendingMessages)
{
    if (!AreWeMember()) {
        return;
    }

    std::set<uint256> justifyFor;

    for (const auto& m : members) {
        if (m->bad) {
            continue;
        }
        if (m->badMemberVotes.size() >= params.dkgBadVotesThreshold) {
            LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: %s marked as bad as %d other members voted for this\n",
                __func__, m->dmn->proTxHash.ToString(), m->badMemberVotes.size());
            MarkBadMember(m->idx);
            continue;
        }
        if (m->complaints.empty()) {
            continue;
        }
        if (m->complaints.size() != 1) {
            LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: %s sent multiple complaints\n",
                __func__, m->dmn->proTxHash.ToString());
            MarkBadMember(m->idx);
            continue;
        }

        auto& qc = complaints.at(*m->complaints.begin());
        if (qc.complainForMembers[myIdx]) {
            justifyFor.emplace(qc.proTxHash);
        }
    }

    if (!justifyFor.empty()) {
        SendJustification(pendingMessages, justifyFor);
    }
}

void CDKGSession::SendJustification(CDKGPendingMessages& pendingMessages, const std::set<uint256>& forMembers)
{
    assert(AreWeMember());

    LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: sending justification for %d members\n",
        __func__, forMembers.size());

    CDKGJustification qj;
    qj.llmqType = (uint8_t)params.type;
    qj.quorumHash = pindexQuorum->GetBlockHash();
    qj.proTxHash = myProTxHash;
    qj.contributions.reserve(forMembers.size());

    for (size_t i = 0; i < members.size(); i++) {
        auto& m = members[i];
        if (!forMembers.count(m->dmn->proTxHash)) {
            continue;
        }
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: justifying for %s\n",
            __func__, m->dmn->proTxHash.ToString());

        CBLSSecretKey skContribution = skContributions[i];

        if (i != myIdx && ShouldSimulateError("justify-lie")) {
            LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: lying for %s\n",
                __func__, m->dmn->proTxHash.ToString());
            skContribution.MakeNewKey();
        }

        qj.contributions.emplace_back(i, skContribution);
    }

    if (ShouldSimulateError("justify-omit")) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: omitting\n", __func__);
        return;
    }

    qj.sig = activeMasternodeInfo.blsKeyOperator->Sign(qj.GetSignHash());

    quorumDKGDebugManager->UpdateLocalSessionStatus(params.type, [&](CDKGDebugSessionStatus& status) {
        status.sentJustification = true;
        return true;
    });

    pendingMessages.PushPendingMessage(-1, qj);
}

// only performs cheap verifications, but not the signature of the message. this is checked with batched verification
bool CDKGSession::PreVerifyMessage(const uint256& hash, const CDKGJustification& qj, bool& retBan) const
{
    retBan = false;

    if (qj.quorumHash != pindexQuorum->GetBlockHash()) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: justification for wrong quorum, rejecting\n", __func__);
        return false;
    }

    auto member = GetMember(qj.proTxHash);
    if (!member) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: justifier not a member of this quorum, rejecting justification\n", __func__);
        retBan = true;
        return false;
    }

    if (qj.contributions.empty()) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: justification with no contributions\n", __func__);
        retBan = true;
        return false;
    }

    std::set<size_t> contributionsSet;
    for (const auto& p : qj.contributions) {
        if (p.first > members.size()) {
            LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: invalid contribution index\n", __func__);
            retBan = true;
            return false;
        }

        if (!contributionsSet.emplace(p.first).second) {
            LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: duplicate contribution index\n", __func__);
            retBan = true;
            return false;
        }

        auto& skShare = p.second;
        if (!skShare.IsValid()) {
            LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: invalid contribution\n", __func__);
            retBan = true;
            return false;
        }
    }

    if (member->justifications.size() >= 2) {
        // don't do any further processing if we got more than 1 valid justification already
        // this is a DoS protection against members sending multiple justifications with valid signatures to us
        // we must bail out before any expensive BLS verification happens
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: dropping justification from %s as we already got %d justifications\n",
            __func__, member->dmn->proTxHash.ToString(), member->justifications.size());
        return false;
    }

    return true;
}

void CDKGSession::ReceiveMessage(const uint256& hash, const CDKGJustification& qj, bool& retBan)
{
    retBan = false;

    LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: received justification from %s\n",
        __func__, qj.proTxHash.ToString());

    auto member = GetMember(qj.proTxHash);

    {
        LOCK(invCs);

        if (member->justifications.size() >= 2) {
            // only relay up to 2 justifications, that's enough to let the other members know about his bad behavior
            return;
        }

        justifications.emplace(hash, qj);
        member->justifications.emplace(hash);

        // we always relay, even if further verification fails
        CInv inv(MSG_QUORUM_JUSTIFICATION, hash);
        invSet.emplace(inv);
        RelayInvToParticipants(inv);

        quorumDKGDebugManager->UpdateLocalMemberStatus(params.type, member->idx, [&](CDKGDebugMemberStatus& status) {
            status.receivedJustification = true;
            return true;
        });

        if (member->justifications.size() > 1) {
            // don't do any further processing if we got more than 1 justification. we already relayed it,
            // so others know about his bad behavior
            LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: %s did send multiple justifications\n",
                __func__, member->dmn->proTxHash.ToString());
            MarkBadMember(member->idx);
            return;
        }

        if (member->bad) {
            // we locally determined him to be bad (sent none or more then one contributions)
            // don't give him a second chance (but we relay the justification in case other members disagree)
            return;
        }
    }

    for (const auto& p : qj.contributions) {
        auto& member2 = members[p.first];

        if (!member->complaintsFromOthers.count(member2->dmn->proTxHash)) {
            LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: got justification from %s for %s even though he didn't complain\n",
                __func__, member->dmn->proTxHash.ToString(), member2->dmn->proTxHash.ToString());
            MarkBadMember(member->idx);
        }
    }
    if (member->bad) {
        return;
    }

    cxxtimer::Timer t1(true);

    std::list<std::future<bool>> futures;
    for (const auto& p : qj.contributions) {
        auto& member2 = members[p.first];
        auto& skContribution = p.second;

        // watch out to not bail out before these async calls finish (they rely on valid references)
        futures.emplace_back(blsWorker.AsyncVerifyContributionShare(member2->id, receivedVvecs[member->idx], skContribution));
    }
    auto resultIt = futures.begin();
    for (const auto& p : qj.contributions) {
        auto& member2 = members[p.first];
        auto& skContribution = p.second;

        bool result = (resultIt++)->get();
        if (!result) {
            LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: %s did send an invalid justification for %s\n",
                __func__, member->dmn->proTxHash.ToString(), member2->dmn->proTxHash.ToString());
            MarkBadMember(member->idx);
        } else {
            LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: %s justified for %s\n",
                __func__, member->dmn->proTxHash.ToString(), member2->dmn->proTxHash.ToString());
            if (AreWeMember() && member2->id == myId) {
                receivedSkContributions[member->idx] = skContribution;
                member->weComplain = false;

                dkgManager.WriteVerifiedSkContribution(params.type, pindexQuorum, member->dmn->proTxHash, skContribution);
            }
            member->complaintsFromOthers.erase(member2->dmn->proTxHash);
        }
    }

    int receivedCount = 0;
    int expectedCount = 0;

    for (const auto& m : members) {
        if (!m->justifications.empty()) {
            receivedCount++;
        }

        if (m->someoneComplain) {
            expectedCount++;
        }
    }

    LogPrint(BCLog::BENCHMARK, "CDKGSession::%s: verified justification: received=%d/%d time=%d\n",
        __func__, receivedCount, expectedCount, t1.count());
}

void CDKGSession::VerifyAndCommit(CDKGPendingMessages& pendingMessages)
{
    if (!AreWeMember()) {
        return;
    }

    std::vector<size_t> badMembers;
    std::vector<size_t> openComplaintMembers;

    for (const auto& m : members) {
        if (m->bad) {
            badMembers.emplace_back(m->idx);
            continue;
        }
        if (!m->complaintsFromOthers.empty()) {
            MarkBadMember(m->idx);
            openComplaintMembers.emplace_back(m->idx);
        }
    }

    if (!badMembers.empty() || !openComplaintMembers.empty()) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: verification result:\n", __func__);
    }
    if (!badMembers.empty()) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: members previously determined as bad:\n", __func__);
        for (const auto& idx : badMembers) {
            LogPrint(BCLog::LLMQDKG, "CDKGSession::%s:     %s\n", __func__, members[idx]->dmn->proTxHash.ToString());
        }
    }
    if (!openComplaintMembers.empty()) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: members with open complaints and now marked as bad:\n", __func__);
        for (const auto& idx : openComplaintMembers) {
            LogPrint(BCLog::LLMQDKG, "CDKGSession::%s:     %s\n", __func__, members[idx]->dmn->proTxHash.ToString());
        }
    }

    SendCommitment(pendingMessages);
}

void CDKGSession::SendCommitment(CDKGPendingMessages& pendingMessages)
{
    assert(AreWeMember());

    LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: sending commitment\n", __func__);

    CDKGPrematureCommitment qc(params);
    qc.llmqType = (uint8_t)params.type;
    qc.quorumHash = pindexQuorum->GetBlockHash();
    qc.proTxHash = myProTxHash;

    for (size_t i = 0; i < members.size(); i++) {
        auto& m = members[i];
        if (!m->bad) {
            qc.validMembers[i] = true;
        }
    }

    if (qc.CountValidMembers() < params.minSize) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: not enough valid members. not sending commitment\n", __func__);
        return;
    }

    if (ShouldSimulateError("commit-omit")) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: omitting\n", __func__);
        return;
    }

    cxxtimer::Timer timerTotal(true);

    cxxtimer::Timer t1(true);
    std::vector<uint16_t> memberIndexes;
    std::vector<BLSVerificationVectorPtr> vvecs;
    BLSSecretKeyVector skContributions;
    if (!dkgManager.GetVerifiedContributions(params.type, pindexQuorum, qc.validMembers, memberIndexes, vvecs, skContributions)) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: failed to get valid contributions\n", __func__);
        return;
    }

    BLSVerificationVectorPtr vvec = cache.BuildQuorumVerificationVector(::SerializeHash(memberIndexes), vvecs);
    if (vvec == nullptr) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: failed to build quorum verification vector\n", __func__);
        return;
    }
    t1.stop();

    cxxtimer::Timer t2(true);
    CBLSSecretKey skShare = cache.AggregateSecretKeys(::SerializeHash(memberIndexes), skContributions);
    if (!skShare.IsValid()) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: failed to build own secret share\n", __func__);
        return;
    }
    t2.stop();

    LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: pubKeyShare=%s\n", __func__, skShare.GetPublicKey().ToString());

    cxxtimer::Timer t3(true);
    qc.quorumPublicKey = (*vvec)[0];
    qc.quorumVvecHash = ::SerializeHash(*vvec);

    int lieType = -1;
    if (ShouldSimulateError("commit-lie")) {
        lieType = GetRandInt(5);
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: lying on commitment. lieType=%d\n", __func__, lieType);
    }

    if (lieType == 0) {
        CBLSSecretKey k;
        k.MakeNewKey();
        qc.quorumPublicKey = k.GetPublicKey();
    } else if (lieType == 1) {
        (*qc.quorumVvecHash.begin())++;
    }

    uint256 commitmentHash = CLLMQUtils::BuildCommitmentHash(qc.llmqType, qc.quorumHash, qc.validMembers, qc.quorumPublicKey, qc.quorumVvecHash);

    if (lieType == 2) {
        (*commitmentHash.begin())++;
    }

    qc.sig = activeMasternodeInfo.blsKeyOperator->Sign(commitmentHash);
    qc.quorumSig = skShare.Sign(commitmentHash);

    if (lieType == 3) {
        std::vector<unsigned char> buf;
        qc.sig.GetBuf(buf);
        buf[5]++;
        qc.sig.SetBuf(buf);
    } else if (lieType == 4) {
        std::vector<unsigned char> buf;
        qc.quorumSig.GetBuf(buf);
        buf[5]++;
        qc.quorumSig.SetBuf(buf);
    }

    t3.stop();
    timerTotal.stop();

    LogPrint(BCLog::BENCHMARK, "CDKGSession::%s: built premature commitment. time1=%d, time2=%d, time3=%d, totalTime=%d\n",
        __func__, t1.count(), t2.count(), t3.count(), timerTotal.count());

    quorumDKGDebugManager->UpdateLocalSessionStatus(params.type, [&](CDKGDebugSessionStatus& status) {
        status.sentPrematureCommitment = true;
        return true;
    });

    pendingMessages.PushPendingMessage(-1, qc);
}

// only performs cheap verifications, but not the signature of the message. this is checked with batched verification
bool CDKGSession::PreVerifyMessage(const uint256& hash, const CDKGPrematureCommitment& qc, bool& retBan) const
{
    cxxtimer::Timer t1(true);

    retBan = false;

    if (qc.quorumHash != pindexQuorum->GetBlockHash()) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: commitment for wrong quorum, rejecting\n", __func__);
        return false;
    }

    auto member = GetMember(qc.proTxHash);
    if (!member) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: committer not a member of this quorum, rejecting premature commitment\n", __func__);
        retBan = true;
        return false;
    }

    if (qc.validMembers.size() != (size_t)params.size) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: invalid validMembers bitset size\n", __func__);
        retBan = true;
        return false;
    }

    if (qc.CountValidMembers() < params.minSize) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: invalid validMembers count. validMembersCount=%d\n", __func__, qc.CountValidMembers());
        retBan = true;
        return false;
    }
    if (!qc.sig.IsValid()) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: invalid membersSig\n", __func__);
        retBan = true;
        return false;
    }
    if (!qc.quorumSig.IsValid()) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: invalid quorumSig\n", __func__);
        retBan = true;
        return false;
    }

    for (size_t i = members.size(); i < params.size; i++) {
        if (qc.validMembers[i]) {
            retBan = true;
            LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: invalid validMembers bitset. bit %d should not be set\n", __func__, i);
            return false;
        }
    }

    if (member->prematureCommitments.size() >= 2) {
        // don't do any further processing if we got more than 1 valid commitment already
        // this is a DoS protection against members sending multiple commitments with valid signatures to us
        // we must bail out before any expensive BLS verification happens
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: dropping commitment from %s as we already got %d commitments\n",
            __func__, member->dmn->proTxHash.ToString(), member->prematureCommitments.size());
        return false;
    }

    return true;
}

void CDKGSession::ReceiveMessage(const uint256& hash, const CDKGPrematureCommitment& qc, bool& retBan)
{
    retBan = false;

    cxxtimer::Timer t1(true);

    LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: received premature commitment from %s. validMembers=%d\n",
        __func__, qc.proTxHash.ToString(), qc.CountValidMembers());

    auto member = GetMember(qc.proTxHash);

    {
        LOCK(invCs);

        // keep track of ALL commitments but only relay valid ones (or if we couldn't build the vvec)
        // relaying is done further down
        prematureCommitments.emplace(hash, qc);
        member->prematureCommitments.emplace(hash);
    }

    std::vector<uint16_t> memberIndexes;
    std::vector<BLSVerificationVectorPtr> vvecs;
    BLSSecretKeyVector skContributions;
    BLSVerificationVectorPtr quorumVvec;
    if (dkgManager.GetVerifiedContributions(params.type, pindexQuorum, qc.validMembers, memberIndexes, vvecs, skContributions)) {
        quorumVvec = cache.BuildQuorumVerificationVector(::SerializeHash(memberIndexes), vvecs);
    }

    if (quorumVvec == nullptr) {
        LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: failed to build quorum verification vector. skipping full verification\n", __func__);
        // we might be the unlucky one who didn't receive all contributions, but we still have to relay
        // the premature commitment as others might be luckier
    } else {
        // we got all information that is needed to verify everything (even though we might not be a member of the quorum)
        // if any of this verification fails, we won't relay this message. This ensures that invalid messages are lost
        // in the network. Nodes relaying such invalid messages to us are not punished as they might have not known
        // all contributions. We only handle up to 2 commitments per member, so a DoS shouldn't be possible

        if ((*quorumVvec)[0] != qc.quorumPublicKey) {
            LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: calculated quorum public key does not match\n", __func__);
            return;
        }
        uint256 vvecHash = ::SerializeHash(*quorumVvec);
        if (qc.quorumVvecHash != vvecHash) {
            LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: calculated quorum vvec hash does not match\n", __func__);
            return;
        }

        CBLSPublicKey pubKeyShare = cache.BuildPubKeyShare(::SerializeHash(std::make_pair(memberIndexes, member->id)), quorumVvec, member->id);
        if (!pubKeyShare.IsValid()) {
            LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: failed to calculate public key share\n", __func__);
            return;
        }

        if (!qc.quorumSig.VerifyInsecure(pubKeyShare, qc.GetSignHash())) {
            LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: failed to verify quorumSig\n", __func__);
            return;
        }
    }

    LOCK(invCs);
    validCommitments.emplace(hash);

    CInv inv(MSG_QUORUM_PREMATURE_COMMITMENT, hash);
    invSet.emplace(inv);
    RelayInvToParticipants(inv);

    quorumDKGDebugManager->UpdateLocalMemberStatus(params.type, member->idx, [&](CDKGDebugMemberStatus& status) {
        status.receivedPrematureCommitment = true;
        return true;
    });

    int receivedCount = 0;
    for (const auto& m : members) {
        if (!m->prematureCommitments.empty()) {
            receivedCount++;
        }
    }

    t1.stop();

    LogPrint(BCLog::BENCHMARK, "CDKGSession::%s: verified premature commitment. received=%d/%d, time=%d\n",
        __func__, receivedCount, members.size(), t1.count());
}

std::vector<CFinalCommitment> CDKGSession::FinalizeCommitments()
{
    if (!AreWeMember()) {
        return {};
    }

    cxxtimer::Timer totalTimer(true);

    typedef std::vector<bool> Key;
    std::map<Key, std::vector<CDKGPrematureCommitment>> commitmentsMap;

    for (const auto& p : prematureCommitments) {
        auto& qc = p.second;
        if (!validCommitments.count(p.first)) {
            continue;
        }

        // should have been verified before
        assert(qc.CountValidMembers() >= params.minSize);

        auto it = commitmentsMap.find(qc.validMembers);
        if (it == commitmentsMap.end()) {
            it = commitmentsMap.emplace(qc.validMembers, std::vector<CDKGPrematureCommitment>()).first;
        }

        it->second.emplace_back(qc);
    }

    std::vector<CFinalCommitment> finalCommitments;
    for (const auto& p : commitmentsMap) {
        auto& cvec = p.second;
        if (cvec.size() < params.minSize) {
            // commitment was signed by a minority
            continue;
        }

        std::vector<CBLSId> signerIds;
        std::vector<CBLSSignature> thresholdSigs;

        auto& first = cvec[0];

        CFinalCommitment fqc(params, first.quorumHash);
        fqc.validMembers = first.validMembers;
        fqc.quorumPublicKey = first.quorumPublicKey;
        fqc.quorumVvecHash = first.quorumVvecHash;

        uint256 commitmentHash = CLLMQUtils::BuildCommitmentHash(fqc.llmqType, fqc.quorumHash, fqc.validMembers, fqc.quorumPublicKey, fqc.quorumVvecHash);

        std::vector<CBLSSignature> aggSigs;
        std::vector<CBLSPublicKey> aggPks;
        aggSigs.reserve(cvec.size());
        aggPks.reserve(cvec.size());

        for (size_t i = 0; i < cvec.size(); i++) {
            auto& qc = cvec[i];

            if (qc.quorumPublicKey != first.quorumPublicKey || qc.quorumVvecHash != first.quorumVvecHash) {
                LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: quorumPublicKey or quorumVvecHash does not match, skipping\n", __func__);
                continue;
            }

            size_t signerIndex = membersMap[qc.proTxHash];
            const auto& m = members[signerIndex];

            fqc.signers[signerIndex] = true;
            aggSigs.emplace_back(qc.sig);
            aggPks.emplace_back(m->dmn->pdmnState->pubKeyOperator.Get());

            signerIds.emplace_back(m->id);
            thresholdSigs.emplace_back(qc.quorumSig);
        }

        cxxtimer::Timer t1(true);
        fqc.membersSig = CBLSSignature::AggregateSecure(aggSigs, aggPks, commitmentHash);
        t1.stop();

        cxxtimer::Timer t2(true);
        if (!fqc.quorumSig.Recover(thresholdSigs, signerIds)) {
            LogPrint(BCLog::LLMQDKG, "CDKGSession::%s: failed to recover quorum sig\n", __func__);
            continue;
        }
        t2.stop();

        finalCommitments.emplace_back(fqc);

        LogPrint(BCLog::BENCHMARK, "CDKGSession::%s: final commitment: validMembers=%d, signers=%d, quorumPublicKey=%s, time1=%d, time2=%d\n",
            __func__, fqc.CountValidMembers(), fqc.CountSigners(), fqc.quorumPublicKey.ToString(),
            t1.count(), t2.count());
    }

    return finalCommitments;
}

CDKGMember* CDKGSession::GetMember(const uint256& proTxHash) const
{
    auto it = membersMap.find(proTxHash);
    if (it == membersMap.end()) {
        return nullptr;
    }
    return members[it->second].get();
}

void CDKGSession::MarkBadMember(size_t idx)
{
    auto member = members.at(idx).get();
    if (member->bad) {
        return;
    }
    quorumDKGDebugManager->UpdateLocalMemberStatus(params.type, idx, [&](CDKGDebugMemberStatus& status) {
        status.bad = true;
        return true;
    });
    member->bad = true;
}

void CDKGSession::RelayInvToParticipants(const CInv& inv) const
{
    LOCK(invCs);
    g_connman->ForEachNode([&](CNode* pnode) {
        bool relay = false;
        if (pnode->qwatch) {
            relay = true;
        } else if (!pnode->verifiedProRegTxHash.IsNull() && membersMap.count(pnode->verifiedProRegTxHash)) {
            relay = true;
        }
        if (relay) {
            pnode->PushInventory(inv);
        }
    });
}

}
