// Copyright (c) 2014-2019 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "spork.h"

#include "base58.h"
#include "chainparams.h"
#include "validation.h"
#include "messagesigner.h"
#include <key_io.h>
#include "net_processing.h"
#include "netmessagemaker.h"
#include <string>

const std::string CSporkManager::SERIALIZATION_VERSION_STRING = "CSporkManager-Version-2";

#define MAKE_SPORK_DEF(name, defaultValue) CSporkDef{name, defaultValue, #name}
std::vector<CSporkDef> sporkDefs = {
    MAKE_SPORK_DEF(SPORK_2_INSTANTSEND_ENABLED,            4070908800ULL),
    MAKE_SPORK_DEF(SPORK_3_INSTANTSEND_BLOCK_FILTERING,    4070908800ULL),
    MAKE_SPORK_DEF(SPORK_5_INSTANTSEND_MAX_VALUE,          1000),
    MAKE_SPORK_DEF(SPORK_6_NEW_SIGS,                       4070908800ULL),
    MAKE_SPORK_DEF(SPORK_9_SUPERBLOCKS_ENABLED,            4070908800ULL),
    MAKE_SPORK_DEF(SPORK_12_RECONSIDER_BLOCKS,                   0),
    MAKE_SPORK_DEF(SPORK_15_DETERMINISTIC_MNS_ENABLED,     4070908800ULL),
    MAKE_SPORK_DEF(SPORK_16_INSTANTSEND_AUTOLOCKS,         4070908800ULL),
    MAKE_SPORK_DEF(SPORK_17_QUORUM_DKG_ENABLED,            4070908800ULL),
    MAKE_SPORK_DEF(SPORK_19_CHAINLOCKS_ENABLED,            4070908800ULL),
    MAKE_SPORK_DEF(SPORK_20_INSTANTSEND_LLMQ_BASED,        4070908800ULL),
    MAKE_SPORK_DEF(SPORK_25_POS_DISABLED_FLAG,             4070908800ULL),
    MAKE_SPORK_DEF(SPORK_26_POW_DISABLED_FLAG,             4070908800ULL),
    MAKE_SPORK_DEF(SPORK_27_ZENTOSHI_RESERVED0,            4070908800ULL),
    MAKE_SPORK_DEF(SPORK_28_ZENTOSHI_RESERVED1,            4070908800ULL),
    MAKE_SPORK_DEF(SPORK_29_ZENTOSHI_RESERVED2,            4070908800ULL),
    MAKE_SPORK_DEF(SPORK_30_ZENTOSHI_RESERVED3,            4070908800ULL)
};

CSporkManager sporkManager;

CSporkManager::CSporkManager()
{
    for (auto& sporkDef : sporkDefs) {
        sporkDefsById.emplace(sporkDef.sporkId, &sporkDef);
        sporkDefsByName.emplace(sporkDef.name, &sporkDef);
    }
}

bool CSporkManager::SporkValueIsActive(SporkId nSporkID, int64_t &nActiveValueRet) const
{
    LOCK(cs);

    if (!mapSporksActive.count(nSporkID)) return false;

    // calc how many values we have and how many signers vote for every value
    std::unordered_map<int64_t, int> mapValueCounts;
    for (const auto& pair: mapSporksActive.at(nSporkID)) {
        mapValueCounts[pair.second.nValue]++;
        if (mapValueCounts.at(pair.second.nValue) >= nMinSporkKeys) {
            // nMinSporkKeys is always more than the half of the max spork keys number,
            // so there is only one such value and we can stop here
            nActiveValueRet = pair.second.nValue;
            return true;
        }
    }

    return false;
}

void CSporkManager::Clear()
{
    LOCK(cs);
    mapSporksActive.clear();
    mapSporksByHash.clear();
}

void CSporkManager::CheckAndRemove()
{
    LOCK(cs);
    bool fSporkAddressIsSet = !setSporkPubKeyIDs.empty();
    assert(fSporkAddressIsSet);

    auto itActive = mapSporksActive.begin();
    while (itActive != mapSporksActive.end()) {
        auto itSignerPair = itActive->second.begin();
        while (itSignerPair != itActive->second.end()) {
            if (setSporkPubKeyIDs.find(itSignerPair->first) == setSporkPubKeyIDs.end()) {
                mapSporksByHash.erase(itSignerPair->second.GetHash());
                continue;
            }
            if (!itSignerPair->second.CheckSignature(itSignerPair->first, false)) {
                if (!itSignerPair->second.CheckSignature(itSignerPair->first, true)) {
                    mapSporksByHash.erase(itSignerPair->second.GetHash());
                    itActive->second.erase(itSignerPair++);
                    continue;
                }
            }
            ++itSignerPair;
        }
        if (itActive->second.empty()) {
            mapSporksActive.erase(itActive++);
            continue;
        }
        ++itActive;
    }

    auto itByHash = mapSporksByHash.begin();
    while (itByHash != mapSporksByHash.end()) {
        bool found = false;
        for (const auto& signer: setSporkPubKeyIDs) {
            if (itByHash->second.CheckSignature(signer, false) ||
                itByHash->second.CheckSignature(signer, true)) {
                found = true;
                break;
            }
        }
        if (!found) {
            mapSporksByHash.erase(itByHash++);
            continue;
        }
        ++itByHash;
    }
}

void CSporkManager::ProcessSpork(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman& connman)
{
    if(fLiteMode) return; // disable all Dash specific functionality

    if (strCommand == NetMsgType::SPORK) {

        CSporkMessage spork;
        vRecv >> spork;

        uint256 hash = spork.GetHash();

        std::string strLogMsg;
        {
            LOCK(cs_main);
            connman.RemoveAskFor(hash);
            if(!::ChainActive().Tip()) return;
            strLogMsg = strprintf("SPORK -- hash: %s id: %d value: %10d bestHeight: %d peer=%d", hash.ToString(), spork.nSporkID, spork.nValue, ::ChainActive().Height(), pfrom->GetId());
        }

        if (spork.nTimeSigned > GetAdjustedTime() + 2 * 60 * 60) {
            LOCK(cs_main);
            LogPrint(BCLog::SPORK, "CSporkManager::ProcessSpork -- ERROR: too far into the future\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

        CKeyID keyIDSigner;
        bool fSpork6IsActive = IsSporkActive(SPORK_6_NEW_SIGS);
        if (!spork.GetSignerKeyID(keyIDSigner, fSpork6IsActive) || !setSporkPubKeyIDs.count(keyIDSigner)) {
            // Note: unlike for other messages we have to check for new format even with SPORK_6_NEW_SIGS
            // inactive because SPORK_6_NEW_SIGS default is OFF and it is not the first spork to sync
            // (and even if it would, spork order can't be guaranteed anyway).
            if (!spork.GetSignerKeyID(keyIDSigner, !fSpork6IsActive) || !setSporkPubKeyIDs.count(keyIDSigner)) {
                LOCK(cs_main);
                LogPrint(BCLog::SPORK, "CSporkManager::ProcessSpork -- ERROR: invalid signature\n");
                Misbehaving(pfrom->GetId(), 100);
                return;
            }
        }

        {
            LOCK(cs); // make sure to not lock this together with cs_main
            if (mapSporksActive.count(spork.nSporkID)) {
                if (mapSporksActive[spork.nSporkID].count(keyIDSigner)) {
                    if (mapSporksActive[spork.nSporkID][keyIDSigner].nTimeSigned >= spork.nTimeSigned) {
                        LogPrint(BCLog::SPORK, "%s seen\n", strLogMsg);
                        return;
                    } else {
                        LogPrintf("%s updated\n", strLogMsg);
                    }
                } else {
                    LogPrintf("%s new signer\n", strLogMsg);
                }
            } else {
                LogPrintf("%s new\n", strLogMsg);
            }
        }


        {
            LOCK(cs); // make sure to not lock this together with cs_main
            mapSporksByHash[hash] = spork;
            mapSporksActive[spork.nSporkID][keyIDSigner] = spork;
        }
        spork.Relay(connman);

    } else if (strCommand == NetMsgType::GETSPORKS) {
        LOCK(cs); // make sure to not lock this together with cs_main
        for (const auto& pair : mapSporksActive) {
            for (const auto& signerSporkPair: pair.second) {
                connman.PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::SPORK, signerSporkPair.second));
            }
        }
    }

}

bool CSporkManager::UpdateSpork(SporkId nSporkID, int64_t nValue, CConnman& connman)
{
    CSporkMessage spork = CSporkMessage(nSporkID, nValue, GetAdjustedTime());

    LOCK(cs);

    bool fSpork6IsActive = IsSporkActive(SPORK_6_NEW_SIGS);
    if (!spork.Sign(sporkPrivKey, fSpork6IsActive)) {
        LogPrint(BCLog::SPORK, "CSporkManager::%s -- ERROR: signing failed for spork %d\n", __func__, nSporkID);
        return false;
    }

    CKeyID keyIDSigner;
    if (!spork.GetSignerKeyID(keyIDSigner, fSpork6IsActive) || !setSporkPubKeyIDs.count(keyIDSigner)) {
        LogPrint(BCLog::SPORK, "CSporkManager::UpdateSpork: failed to find keyid for private key\n");
        return false;
    }

    LogPrint(BCLog::SPORK, "CSporkManager::%s -- signed %d %s\n", __func__, nSporkID, spork.GetHash().ToString());

    mapSporksByHash[spork.GetHash()] = spork;
    mapSporksActive[nSporkID][keyIDSigner] = spork;

    spork.Relay(connman);
    return true;
}

bool CSporkManager::IsSporkActive(SporkId nSporkID)
{
    int64_t nSporkValue = GetSporkValue(nSporkID);
    return nSporkValue < GetAdjustedTime();
}

int64_t CSporkManager::GetSporkValue(SporkId nSporkID)
{
    LOCK(cs);

    int64_t nSporkValue = -1;
    if (SporkValueIsActive(nSporkID, nSporkValue)) {
        return nSporkValue;
    }

    auto it = sporkDefsById.find(nSporkID);
    if (it != sporkDefsById.end()) {
        return it->second->defaultValue;
    }

    LogPrint(BCLog::SPORK, "CSporkManager::GetSporkValue -- Unknown Spork ID %d\n", nSporkID);
    return -1;
}

SporkId CSporkManager::GetSporkIDByName(const std::string& strName)
{
    auto it = sporkDefsByName.find(strName);
    if (it == sporkDefsByName.end()) {
        LogPrint(BCLog::SPORK, "CSporkManager::GetSporkIDByName -- Unknown Spork name '%s'\n", strName);
        return SPORK_INVALID;
    }
    return it->second->sporkId;
}

std::string CSporkManager::GetSporkNameByID(SporkId nSporkID)
{
    auto it = sporkDefsById.find(nSporkID);
    if (it == sporkDefsById.end()) {
        LogPrint(BCLog::SPORK, "CSporkManager::GetSporkNameByID -- Unknown Spork ID %d\n", nSporkID);
        return "Unknown";
    }
    return it->second->name;
}

bool CSporkManager::GetSporkByHash(const uint256& hash, CSporkMessage &sporkRet)
{
    LOCK(cs);

    const auto it = mapSporksByHash.find(hash);

    if (it == mapSporksByHash.end())
        return false;

    sporkRet = it->second;

    return true;
}

bool CSporkManager::SetSporkAddress(const std::string& strAddress) {
    LOCK(cs);
    CBitcoinAddress address(strAddress);
    CKeyID keyid;
    if (!address.IsValid() || !address.GetKeyID(keyid)) {
        LogPrint(BCLog::SPORK, "CSporkManager::SetSporkAddress -- Failed to parse spork address\n");
        return false;
    }
    setSporkPubKeyIDs.insert(keyid);
    return true;
}

bool CSporkManager::SetMinSporkKeys(int minSporkKeys)
{
    int maxKeysNumber = setSporkPubKeyIDs.size();
    if ((minSporkKeys <= maxKeysNumber / 2) || (minSporkKeys > maxKeysNumber)) {
        LogPrint(BCLog::SPORK, "CSporkManager::SetMinSporkKeys -- Invalid min spork signers number: %d\n", minSporkKeys);
        return false;
    }
    nMinSporkKeys = minSporkKeys;
    return true;
}

bool CSporkManager::SetPrivKey(const std::string& strPrivKey)
{
    CKey key;
    CPubKey pubKey;
    if(!CMessageSigner::GetKeysFromSecret(strPrivKey, key, pubKey)) {
        LogPrint(BCLog::SPORK, "CSporkManager::SetPrivKey -- Failed to parse private key\n");
        return false;
    }

    if (setSporkPubKeyIDs.find(pubKey.GetID()) == setSporkPubKeyIDs.end()) {
        LogPrint(BCLog::SPORK, "CSporkManager::SetPrivKey -- New private key does not belong to spork addresses\n");
        return false;
    }

    CSporkMessage spork;
    if (!spork.Sign(key, IsSporkActive(SPORK_6_NEW_SIGS))) {
        LogPrint(BCLog::SPORK, "CSporkManager::SetPrivKey -- Test signing failed\n");
        return false;
    }

    // Test signing successful, proceed
    LOCK(cs);
    LogPrint(BCLog::SPORK, "CSporkManager::SetPrivKey -- Successfully initialized as spork signer\n");
    sporkPrivKey = key;
    return true;
}

std::string CSporkManager::ToString() const
{
    LOCK(cs);
    return strprintf("Sporks: %llu", mapSporksActive.size());
}

uint256 CSporkMessage::GetHash() const
{
    return SerializeHash(*this);
}

uint256 CSporkMessage::GetSignatureHash() const
{
    CHashWriter s(SER_GETHASH, 0);
    s << nSporkID;
    s << nValue;
    s << nTimeSigned;
    return s.GetHash();
}

bool CSporkMessage::Sign(const CKey& key, bool fSporkSixActive)
{
    if (!key.IsValid()) {
        LogPrint(BCLog::SPORK, "CSporkMessage::Sign -- signing key is not valid\n");
        return false;
    }

    CKeyID pubKeyId = key.GetPubKey().GetID();
    std::string strError = "";

    if (fSporkSixActive) {
        uint256 hash = GetSignatureHash();

        if(!CHashSigner::SignHash(hash, key, vchSig)) {
            LogPrint(BCLog::SPORK, "CSporkMessage::Sign -- SignHash() failed\n");
            return false;
        }

        if (!CHashSigner::VerifyHash(hash, pubKeyId, vchSig, strError)) {
            LogPrint(BCLog::SPORK, "CSporkMessage::Sign -- VerifyHash() failed, error: %s\n", strError);
            return false;
        }
    } else {
        std::string strMessage = std::to_string(nSporkID) + std::to_string(nValue) + std::to_string(nTimeSigned);

        if(!CMessageSigner::SignMessage(strMessage, vchSig, key)) {
            LogPrint(BCLog::SPORK, "CSporkMessage::Sign -- SignMessage() failed\n");
            return false;
        }

        if(!CMessageSigner::VerifyMessage(pubKeyId, vchSig, strMessage, strError)) {
            LogPrint(BCLog::SPORK, "CSporkMessage::Sign -- VerifyMessage() failed, error: %s\n", strError);
            return false;
        }
    }

    return true;
}

bool CSporkMessage::CheckSignature(const CKeyID& pubKeyId, bool fSporkSixActive) const
{
    std::string strError = "";

    if (fSporkSixActive) {
        uint256 hash = GetSignatureHash();

        if (!CHashSigner::VerifyHash(hash, pubKeyId, vchSig, strError)) {
            // Note: unlike for many other messages when SPORK_6_NEW_SIGS is ON sporks with sigs in old format
            // and newer timestamps should not be accepted, so if we failed here - that's it
            LogPrint(BCLog::SPORK, "CSporkMessage::CheckSignature -- VerifyHash() failed, error: %s\n", strError);
            return false;
        }
    } else {
        std::string strMessage = std::to_string(nSporkID) + std::to_string(nValue) + std::to_string(nTimeSigned);

        if (!CMessageSigner::VerifyMessage(pubKeyId, vchSig, strMessage, strError)){
            // Note: unlike for other messages we have to check for new format even with SPORK_6_NEW_SIGS
            // inactive because SPORK_6_NEW_SIGS default is OFF and it is not the first spork to sync
            // (and even if it would, spork order can't be guaranteed anyway).
            uint256 hash = GetSignatureHash();
            if (!CHashSigner::VerifyHash(hash, pubKeyId, vchSig, strError)) {
                LogPrint(BCLog::SPORK, "CSporkMessage::CheckSignature -- VerifyHash() failed, error: %s\n", strError);
                return false;
            }
        }
    }

    return true;
}

bool CSporkMessage::GetSignerKeyID(CKeyID &retKeyidSporkSigner, bool fSporkSixActive)
{
    CPubKey pubkeyFromSig;
    if (fSporkSixActive) {
         CPubKey::InputScriptType inputScriptType;
         if (!pubkeyFromSig.RecoverCompact(GetSignatureHash(), vchSig, inputScriptType)) {
             return false;
         }
    } else {
        std::string strMessage = std::to_string(nSporkID) + std::to_string(nValue) + std::to_string(nTimeSigned);
         CHashWriter ss(SER_GETHASH, 0);
         ss << strMessageMagic;
         ss << strMessage;
         CPubKey::InputScriptType inputScriptType;
         if (!pubkeyFromSig.RecoverCompact(ss.GetHash(), vchSig, inputScriptType)) {
             return false;
         }
    }
    retKeyidSporkSigner = pubkeyFromSig.GetID();
    return true;
}

void CSporkMessage::Relay(CConnman& connman)
{
    CInv inv(MSG_SPORK, GetHash());
    connman.RelayInv(inv);
}
