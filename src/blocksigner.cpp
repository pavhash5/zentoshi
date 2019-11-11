#include <blocksigner.h>
#include <messagesigner.h>
#include <primitives/block.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <util/strencodings.h>
#include <util/system.h>

typedef std::vector<uint8_t> valtype;

bool SignBlockWithKey(CBlock& block, const CKey& key)
{
    if (!key.Sign(block.GetHash(), block.vchBlockSig))
        return error("%s: failed to sign block hash with key", __func__);

    return true;
}

bool GetKeyIDFromUTXO(const CTxOut& txout, CKeyID& keyID)
{
    int resultType = 0;
    std::vector<valtype> vSolutions;
    txnouttype whichType = Solver(txout.scriptPubKey, vSolutions);

    if (whichType == TX_PUBKEY) {
        resultType = 1;
        keyID = CPubKey(vSolutions[0]).GetID();
    } else if (whichType == TX_PUBKEYHASH) {
        resultType = 2;
        keyID = CKeyID(uint160(vSolutions[0]));
    } else if (whichType == TX_WITNESS_V0_SCRIPTHASH ||
               whichType == TX_WITNESS_V0_KEYHASH) {
        resultType = 3;
        keyID = CKeyID(uint160(vSolutions[0]));
    }
    LogPrintf("GetKeyIDFromUTXO()::Type %d\n", resultType);

    return true;
}

bool SignBlock(CBlock& block, FillableSigningProvider& keystore)
{
    std::vector<valtype> vSolutions;
    CKeyID keyID;
    if (block.IsProofOfWork()) {
        bool fFoundID = false;
        for (const CTxOut& txout :block.vtx[0]->vout) {
            if (!GetKeyIDFromUTXO(txout, keyID))
                continue;
            fFoundID = true;
            break;
        }
        if (!fFoundID)
            return error("%s: failed to find key for PoW", __func__);
    } else {
        if (!GetKeyIDFromUTXO(block.vtx[1]->vout[1], keyID))
            return error("%s: failed to find key for PoS", __func__);
    }

    CKey key;
    if (!keystore.GetKey(keyID, key))
        return error("%s: failed to get key from keystore", __func__);

    return SignBlockWithKey(block, key);
}

bool CheckBlockSignature(const CBlock& block)
{
    std::vector<valtype> vSolutions;

    if (block.IsProofOfWork())
        return block.vchBlockSig.empty();

    if (block.vchBlockSig.empty())
        return error("%s: vchBlockSig is empty!", __func__);

    CPubKey pubkey;
    const CTxOut& txout = block.vtx[1]->vout[1];
    txnouttype whichType = Solver(txout.scriptPubKey, vSolutions);
    if (whichType == TX_PUBKEY || whichType == TX_PUBKEYHASH) {
        valtype& vchPubKey = vSolutions[0];
        pubkey = CPubKey(vchPubKey);
    }

    if (!pubkey.IsValid())
        return error("%s: invalid pubkey %s", __func__, HexStr(pubkey));

    return pubkey.Verify(block.GetHash(), block.vchBlockSig);
}
