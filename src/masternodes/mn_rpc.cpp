// Copyright (c) 2020 The DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <masternodes/masternodes.h>

#include <chainparams.h>
#include <core_io.h>
#include <consensus/validation.h>
#include <net.h>
#include <rpc/client.h>
#include <rpc/server.h>
#include <rpc/protocol.h>
#include <rpc/util.h>
#include <script/script_error.h>
#include <script/sign.h>
#include <univalue/include/univalue.h>
#include <util/validation.h>
#include <validation.h>
#include <version.h>

//#ifdef ENABLE_WALLET
#include <wallet/coincontrol.h>
#include <wallet/rpcwallet.h>
#include <wallet/wallet.h>
//#endif

#include <future>
#include <stdexcept>

#include <boost/algorithm/string.hpp>
#include <boost/assign/list_of.hpp>

extern UniValue createrawtransaction(UniValue const & params, bool fHelp); // in rawtransaction.cpp
extern UniValue fundrawtransaction(UniValue const & params, bool fHelp); // in rpcwallet.cpp
extern UniValue signrawtransaction(UniValue const & params, bool fHelp); // in rawtransaction.cpp
extern UniValue sendrawtransaction(UniValue const & params, bool fHelp); // in rawtransaction.cpp
extern UniValue getnewaddress(UniValue const & params, bool fHelp); // in rpcwallet.cpp
extern bool EnsureWalletIsAvailable(bool avoidException); // in rpcwallet.cpp
extern bool DecodeHexTx(CTransaction & tx, std::string const & strHexTx); // in core_io.h

extern void ScriptPubKeyToJSON(CScript const & scriptPubKey, UniValue & out, bool fIncludeHex); // in rawtransaction.cpp

extern void FundTransaction(CWallet* const pwallet, CMutableTransaction& tx, CAmount& fee_out, int& change_position, UniValue options);

static UniValue fundsignsend(CMutableTransaction & mtx, JSONRPCRequest const & request, CWallet* const pwallet)
{
    CAmount fee;
    int change_position;
    UniValue options(UniValue::VOBJ);
    options.pushKV("changePosition", (uint64_t)mtx.vout.size());
    FundTransaction(pwallet, mtx, fee, change_position, options);

    JSONRPCRequest new_request;
    new_request.id = request.id;
    new_request.URI = request.URI;

    new_request.params.setArray();
    new_request.params.push_back(EncodeHexTx(CTransaction(mtx)));
    UniValue txSigned = signrawtransactionwithwallet(new_request);

    // from "sendrawtransaction"
    {
        CMutableTransaction mtx;
        if (!DecodeHexTx(mtx, txSigned["hex"].get_str()))
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
        CTransactionRef tx(MakeTransactionRef(std::move(mtx)));

        CAmount max_raw_tx_fee = {COIN / 10}; /// @todo check it with 0

        std::string err_string;
        AssertLockNotHeld(cs_main);
        const TransactionError err = BroadcastTransaction(tx, err_string, max_raw_tx_fee, /*relay*/ true, /*wait_callback*/ false);
        if (TransactionError::OK != err) {
            throw JSONRPCTransactionError(err, err_string);
        }
        return tx->GetHash().GetHex();
    }
}


CAmount EstimateMnCreationFee()
{
    // Current height + (1 day blocks) to avoid rejection;
    int targetHeight = ::ChainActive().Height() + 1 + (60 * 60 / Params().GetConsensus().pos.nTargetSpacing);
    return GetMnCreationFee(targetHeight);
}

void FillInputs(UniValue const & inputs, CMutableTransaction & rawTx)
{
    for (unsigned int idx = 0; idx < inputs.size(); idx++)
    {
        const UniValue& input = inputs[idx];
        const UniValue& o = input.get_obj();

        uint256 txid = ParseHashO(o, "txid");

        const UniValue& vout_v = find_value(o, "vout");
        if (!vout_v.isNum())
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, missing vout key");
        int nOutput = vout_v.get_int();
        if (nOutput < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, vout must be positive");

        rawTx.vin.push_back(CTxIn(txid, nOutput));
    }
}

static CWallet* GetWallet(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    EnsureWalletIsAvailable(pwallet, false);
    EnsureWalletIsUnlocked(pwallet);
    return pwallet;
}

/*
 *
 *  Issued by: any
*/
UniValue createmasternode(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWallet(request);

    RPCHelpMan{"createmasternode",
        "\nCreates (and submits to local node and network) a masternode creation transaction with given metadata, spending the given inputs..\n"
        "The first optional argument (may be empty array) is an array of specific UTXOs to spend." +
            HelpRequiringPassphrase(pwallet) + "\n",
        {
            {"inputs", RPCArg::Type::ARR, RPCArg::Optional::OMITTED_NAMED_ARG, "A json array of json objects",
                {
                    {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                        {
                            {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                            {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                        },
                    },
                },
            },
            {"metadata", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                {
                    {"operatorAuthAddress", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Masternode operator auth address (P2PKH only, unique)" },
                    {"collateralAddress", RPCArg::Type::STR, RPCArg::Optional::NO, "Any valid address for keeping collateral amount (any P2PKH or P2WKH address) - used as owner key"},
                },
            },
        },
        RPCResult{
            "\"hex\"                  (string) The hex-encoded raw transaction with signature(s)\n"
        },
        RPCExamples{
            HelpExampleCli("createmasternode", "\"[{\\\"txid\\\":\\\"id\\\",\\\"vout\\\":0}]\" "
                                            "\"{\\\"operatorAuthAddress\\\":\\\"address\\\","
                                               "\\\"collateralAddress\\\":\\\"address\\\""
                                            "}\"")
            + HelpExampleRpc("createmasternode", "\"[{\\\"txid\\\":\\\"id\\\",\\\"vout\\\":0}]\" "
                                          "\"{\\\"operatorAuthAddress\\\":\\\"address\\\","
                                             "\\\"collateralAddress\\\":\\\"address\\\""
                                            "}\"")
        },
    }.Check(request);


    if (pwallet->chain().isInitialBlockDownload()) {
        throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "Cannot create Masternode while still in Initial Block Download");
    }

    RPCTypeCheck(request.params, { UniValue::VARR, UniValue::VOBJ }, true);
    if (request.params[0].isNull() || request.params[1].isNull())
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameters, arguments 1 and 2 must be non-null, and argument 2 expected as object with "
                                                  "{\"operatorAuthAddress\",\"collateralAddress\"}");
    }
    UniValue metaObj = request.params[1].get_obj();
    RPCTypeCheckObj(metaObj, {
                        { "operatorAuthAddress", UniValue::VSTR },
                        { "collateralAddress", UniValue::VSTR }
                    },
                    true, true);

    std::string collateralAddress =         metaObj["collateralAddress"].getValStr();
    std::string operatorAuthAddressBase58 = metaObj["operatorAuthAddress"].getValStr();

    CTxDestination collateralDest = DecodeDestination(collateralAddress);
    if (collateralDest.which() != 1 && collateralDest.which() != 4)
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "collateralAddress (" + collateralAddress + ") does not refer to a P2PKH or P2WPKH address");
    }
    CKeyID ownerAuthKey = collateralDest.which() == 1 ? CKeyID(*boost::get<PKHash>(&collateralDest)) : CKeyID(*boost::get<WitnessV0KeyHash>(&collateralDest));

    CTxDestination operatorDest = operatorAuthAddressBase58 == "" ? collateralDest : DecodeDestination(operatorAuthAddressBase58);
    if (operatorDest.which() != 1 && operatorDest.which() != 4)
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "operatorAuthAddress (" + operatorAuthAddressBase58 + ") does not refer to a P2PKH or P2WPKH address");
    }
    CKeyID operatorAuthKey = operatorDest.which() == 1 ? CKeyID(*boost::get<PKHash>(&operatorDest)) : CKeyID(*boost::get<WitnessV0KeyHash>(&operatorDest)) ;

    {
        auto locked_chain = pwallet->chain().lock();

        if (pmasternodesview->ExistMasternode(CMasternodesView::AuthIndex::ByOwner, ownerAuthKey) ||
            pmasternodesview->ExistMasternode(CMasternodesView::AuthIndex::ByOperator, ownerAuthKey))
        {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Masternode with collateralAddress == " + collateralAddress + " already exists");
        }
        if (pmasternodesview->ExistMasternode(CMasternodesView::AuthIndex::ByOwner, operatorAuthKey) ||
            pmasternodesview->ExistMasternode(CMasternodesView::AuthIndex::ByOperator, operatorAuthKey))
        {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Masternode with operatorAuthAddress == " + EncodeDestination(operatorDest) + " already exists");
        }
    }

    CDataStream metadata(DfTxMarker, SER_NETWORK, PROTOCOL_VERSION);
    metadata << static_cast<unsigned char>(MasternodesTxType::CreateMasternode)
             << static_cast<char>(operatorDest.which()) << operatorAuthKey;

    CScript scriptMeta;
    scriptMeta << OP_RETURN << ToByteVector(metadata);

    CMutableTransaction rawTx;

    FillInputs(request.params[0].get_array(), rawTx);

    rawTx.vout.push_back(CTxOut(EstimateMnCreationFee(), scriptMeta));
    rawTx.vout.push_back(CTxOut(GetMnCollateralAmount(), GetScriptForDestination(collateralDest)));

    return fundsignsend(rawTx, request, pwallet);
}


UniValue resignmasternode(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWallet(request);

    RPCHelpMan{"resignmasternode",
        "\nCreates (and submits to local node and network) a transaction resigning your masternode. Collateral will be unlocked after " + std::to_string(GetMnResignDelay()) + " blocks.\n"
        "The first optional argument (may be empty array) is an array of specific UTXOs to spend. One of UTXO's must belong to the MN's owner (collateral) address" +
            HelpRequiringPassphrase(pwallet) + "\n",
        {
            {"inputs", RPCArg::Type::ARR, RPCArg::Optional::OMITTED_NAMED_ARG, "A json array of json objects. Provide it if you want to spent specific UTXOs",
                {
                    {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                        {
                            {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                            {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                        },
                    },
                },
            },
            {"mn_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The Masternode's ID"},
        },
        RPCResult{
            "\"hex\"                      (string) The hex-encoded raw transaction with signature(s)\n"
        },
        RPCExamples{
            HelpExampleCli("resignmasternode", "\"[{\\\"txid\\\":\\\"id\\\",\\\"vout\\\":0}]\" \"mn_id\"")
            + HelpExampleRpc("resignmasternode", "\"[{\\\"txid\\\":\\\"id\\\",\\\"vout\\\":0}]\" \"mn_id\"")
        },
    }.Check(request);

    if (pwallet->chain().isInitialBlockDownload()) {
        throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "Cannot resign Masternode while still in Initial Block Download");
    }

    RPCTypeCheck(request.params, { UniValue::VARR, UniValue::VSTR }, true);

    std::string const nodeIdStr = request.params[1].getValStr();
    uint256 nodeId = uint256S(nodeIdStr);
    CTxDestination ownerDest;
    {
        auto locked_chain = pwallet->chain().lock();
        auto optIDs = pmasternodesview->AmIOwner();
        if (!optIDs)
        {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("You are not the owner of masternode %s, or it does not exist", nodeIdStr));
        }
        auto nodePtr = pmasternodesview->ExistMasternode(nodeId);
        if (nodePtr->banHeight != -1)
        {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Masternode %s was criminal, banned at height %i by tx %s", nodeIdStr, nodePtr->banHeight, nodePtr->banTx.GetHex()));
        }

        if (nodePtr->resignHeight != -1)
        {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Masternode %s was resigned by tx %s; collateral can be spend at block #%d", nodeIdStr, nodePtr->resignTx.GetHex(), nodePtr->resignHeight + GetMnResignDelay()));
        }
        ownerDest = nodePtr->ownerType == 1 ? CTxDestination(PKHash(nodePtr->ownerAuthAddress)) : CTxDestination(WitnessV0KeyHash(nodePtr->ownerAuthAddress));
    }

    CMutableTransaction rawTx;

    UniValue inputs = request.params[0].get_array();
    if (inputs.size() > 0)
    {
        FillInputs(request.params[0].get_array(), rawTx);
    }
    else
    {
        std::vector<COutput> vecOutputs;
        CCoinControl cctl;
        cctl.m_avoid_address_reuse = false;
        cctl.m_min_depth = 1;
        cctl.m_max_depth = 9999999;
        cctl.matchDestination = ownerDest;
        auto locked_chain = pwallet->chain().lock();
        LOCK(pwallet->cs_wallet);
        pwallet->AvailableCoins(*locked_chain, vecOutputs, true, &cctl, 1, MAX_MONEY, MAX_MONEY, 1);

        if (vecOutputs.size() == 0)
        {
            throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, strprintf("Can't find any UTXO's for ownerAuthAddress (%s). Send some coins and try again!", EncodeDestination(ownerDest)));
        }
        rawTx.vin.push_back(CTxIn(vecOutputs[0].tx->GetHash(), vecOutputs[0].i));
    }

    CDataStream metadata(DfTxMarker, SER_NETWORK, PROTOCOL_VERSION);
    metadata << static_cast<unsigned char>(MasternodesTxType::ResignMasternode)
             << nodeId;

    CScript scriptMeta;
    scriptMeta << OP_RETURN << ToByteVector(metadata);

    rawTx.vout.push_back(CTxOut(0, scriptMeta));

    return fundsignsend(rawTx, request, pwallet);
}


// Here (but not a class method) just by similarity with other '..ToJSON'
UniValue mnToJSON(CMasternode const & node)
{
    UniValue ret(UniValue::VOBJ);
    ret.pushKV("ownerAuthAddress", EncodeDestination(node.ownerType == 1 ? CTxDestination(PKHash(node.ownerAuthAddress)) : CTxDestination(WitnessV0KeyHash(node.ownerAuthAddress))));
    ret.pushKV("operatorAuthAddress", EncodeDestination(node.operatorType == 1 ? CTxDestination(PKHash(node.operatorAuthAddress)) : CTxDestination(WitnessV0KeyHash(node.operatorAuthAddress))));

    ret.pushKV("creationHeight", node.creationHeight);
    ret.pushKV("resignHeight", node.resignHeight);
    ret.pushKV("resignTx", node.resignTx.GetHex());
    ret.pushKV("banHeight", node.banHeight);
    ret.pushKV("banTx", node.banTx.GetHex());
    ret.pushKV("state", CMasternode::GetHumanReadableState(node.GetState()));
    ret.pushKV("mintedBlocks", (uint64_t)node.mintedBlocks);

    /// @todo add unlock height and|or real resign height

    return ret;
}

UniValue listmasternodes(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWallet(request);

    RPCHelpMan{"listmasternodes",
        "\nReturns information about specified masternodes (or all, if list of ids is empty).\n",
        {
            {"list", RPCArg::Type::ARR, RPCArg::Optional::OMITTED_NAMED_ARG, "A json array of masternode ids",
                {
                    {"mn_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Masternode's id"},
                },
            },
            {"verbose", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Flag for verbose list (default = true), otherwise only ids and statuses listed"},
        },
        RPCResult{
            "{id:{...},...}     (array) Json object with masternodes information\n"
        },
        RPCExamples{
            HelpExampleCli("listmasternodes", "\"[mn_id]\" False")
            + HelpExampleRpc("listmasternodes", "\"[mn_id]\" False")
        },
    }.Check(request);

    RPCTypeCheck(request.params, { UniValue::VARR, UniValue::VBOOL }, true);

    UniValue inputs(UniValue::VARR);
    if (request.params.size() > 0)
    {
        inputs = request.params[0].get_array();
    }
    bool verbose = true;
    if (request.params.size() > 1)
    {
        verbose = request.params[1].get_bool();
    }

    auto locked_chain = pwallet->chain().lock();

    UniValue ret(UniValue::VOBJ);
    CMasternodes const & mns = pmasternodesview->GetMasternodes();
    if (inputs.empty())
    {
        // Dumps all!
        for (auto it = mns.begin(); it != mns.end(); ++it)
        {
            if (it->second != CMasternode())
                ret.pushKV(it->first.GetHex(), verbose ? mnToJSON(it->second) : CMasternode::GetHumanReadableState(it->second.GetState()));
        }
    }
    else
    {
        for (size_t idx = 0; idx < inputs.size(); ++idx)
        {
            uint256 id = ParseHashV(inputs[idx], "masternode id");
            auto const & node = pmasternodesview->ExistMasternode(id);
            if (node && *node != CMasternode())
            {
                ret.pushKV(id.GetHex(), verbose ? mnToJSON(*node) : CMasternode::GetHumanReadableState(node->GetState()));
            }
        }
    }
    return ret;
}

UniValue listcriminalproofs(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWallet(request);

    RPCHelpMan{"listcriminalproofs",
        "\nReturns information about criminal proofs (pairs of signed blocks by one MN from different forks).\n",
        {
        },
        RPCResult{
            "{id:{block1, block2},...}     (array) Json objects with block pairs\n"
        },
        RPCExamples{
            HelpExampleCli("listcriminalproofs", "")
            + HelpExampleRpc("listcriminalproofs", "")
        },
    }.Check(request);

    auto locked_chain = pwallet->chain().lock();

    UniValue ret(UniValue::VOBJ);
    auto const proofs = pmasternodesview->GetUnpunishedCriminals();
    for (auto const & proof : proofs) {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("hash1", proof.second.blockHeader.GetHash().ToString());
        obj.pushKV("height1", proof.second.blockHeader.height);
        obj.pushKV("hash2", proof.second.conflictBlockHeader.GetHash().ToString());
        obj.pushKV("height2", proof.second.conflictBlockHeader.height);
        obj.pushKV("mintedBlocks", proof.second.blockHeader.mintedBlocks);
        ret.pushKV(proof.first.ToString(), obj);
    }
    return ret;
}


static const CRPCCommand commands[] =
{ //  category          name                        actor (function)            params
  //  ----------------- ------------------------    -----------------------     ----------
  { "masternodes",      "createmasternode",         &createmasternode,          { "inputs", "metadata" }  },
  { "masternodes",      "resignmasternode",         &resignmasternode,          { "inputs", "mn_id" }  },
  { "masternodes",      "listmasternodes",          &listmasternodes,           { "list", "verbose" } },
  { "masternodes",      "listcriminalproofs",       &listcriminalproofs,        { } },
};

void RegisterMasternodesRPCCommands(CRPCTable &tableRPC)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        tableRPC.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
