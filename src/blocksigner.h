#ifndef BLOCKSIGNER_H
#define BLOCKSIGNER_H

#include <script/signingprovider.h>

class CBlock;
class CPubKey;
class CKey;
class SigningProvider;

bool SignBlockWithKey(CBlock& block, const CKey& key);
bool SignBlock(CBlock& block, FillableSigningProvider& keystore);
bool CheckBlockSignature(const CBlock& block);

#endif // BLOCKSIGNER_H
