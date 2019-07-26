// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef SPORK_H
#define SPORK_H

#include <hash.h>
#include <key.h>
#include <net.h>
#include <util/strencodings.h>

class CSporkMessage;
class CSporkManager;
class CKey;

namespace Spork {

static const int SPORK_START                                   = 10001;

enum {
	SPORK_2_INSTANTSEND_ENABLED                            = SPORK_START,
	SPORK_3_INSTANTSEND_BLOCK_FILTERING                    = 10002,
	SPORK_5_INSTANTSEND_MAX_VALUE                          = 10004,
	SPORK_6_NEW_SIGS                                       = 10005,
	SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT                 = 10007,
	SPORK_9_SUPERBLOCKS_ENABLED                            = 10008,
	SPORK_10_MASTERNODE_PAY_UPDATED_NODES                  = 10009,
	SPORK_12_RECONSIDER_BLOCKS                             = 10011,
	SPORK_14_REQUIRE_SENTINEL_FLAG                         = 10013,
	SPORK_15_DETERMINISTIC_MNS_ENABLED                     = 10014,
	SPORK_16_INSTANTSEND_AUTOLOCKS                         = 10015,
	SPORK_17_QUORUM_DKG_ENABLED                            = 10016,
	SPORK_25_POS_DISABLED_FLAG                             = 10025,
	SPORK_26_POW_DISABLED_FLAG                             = 10026,
	SPORK_END
};

}

extern std::map<int, int64_t> mapSporkDefaults;
extern std::map<uint256, CSporkMessage> mapSporks;
extern CSporkManager sporkManager;

//
// Spork classes
// Keep track of all of the network spork settings
//

class CSporkMessage
{
private:
    std::vector<unsigned char> vchSig;

public:
    int nSporkID;
    int64_t nValue;
    int64_t nTimeSigned;

    CSporkMessage(int nSporkID, int64_t nValue, int64_t nTimeSigned) :
        nSporkID(nSporkID),
        nValue(nValue),
        nTimeSigned(nTimeSigned)
        {}

    CSporkMessage() :
        nSporkID(0),
        nValue(0),
        nTimeSigned(0)
        {}


    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(nSporkID);
        READWRITE(nValue);
        READWRITE(nTimeSigned);
        READWRITE(vchSig);
    }

    uint256 GetHash() const
    {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << nSporkID;
        ss << nValue;
        ss << nTimeSigned;
        return ss.GetHash();
    }

    bool Sign(std::string strSignKey);
    bool CheckSignature();
    void Relay(CConnman *connman);
};


class CSporkManager
{
private:
    static const std::string SERIALIZATION_VERSION_STRING;
	
    mutable CCriticalSection cs;
    std::map<uint256, CSporkMessage> mapSporksByHash;
    std::vector<unsigned char> vchSig;
    std::string strMasterPrivKey;
    std::map<int, CSporkMessage> mapSporksActive;
    std::set<CKeyID> setSporkPubKeyIDs;
    int nMinSporkKeys;
    CKey sporkPrivKey;

public:
    using Executor = std::function<void(void)>;
    CSporkManager() {}

    void ProcessSpork(CNode* pfrom, const std::string &strCommand, CDataStream& vRecv, CConnman *connman);
    bool UpdateSpork(int nSporkID, int64_t nValue, CConnman *connman);
    void ExecuteSpork(int nSporkID, int nValue);

    bool IsSporkActive(int nSporkID);
    int64_t GetSporkValue(int nSporkID);
    int GetSporkIDByName(std::string strName);
    std::string GetSporkNameByID(int nSporkID);

    bool GetSporkByHash(const uint256& hash, CSporkMessage &sporkRet);
    bool SetSporkAddress(const std::string& strAddress);
    bool SetMinSporkKeys(int minSporkKeys);
    bool SetPrivKey(std::string strPrivKey);
};

#endif
