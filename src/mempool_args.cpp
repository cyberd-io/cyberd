// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <mempool_args.h>

#include <kernel/mempool_options.h>

#include <chainparams.h>
#include <consensus/amount.h>
#include <tinyformat.h>
#include <util/error.h>
#include <util/moneystr.h>
#include <util/system.h>
#include <util/translation.h>

#include <chrono>
#include <memory>

using kernel::MemPoolOptions;

std::optional<bilingual_str>
ApplyArgsManOptions(const ArgsManager &argsman, const CChainParams &chainparams,
                    MemPoolOptions &mempool_opts) {
    mempool_opts.check_ratio =
        argsman.GetIntArg("-checkmempool", mempool_opts.check_ratio);

    if (auto mb = argsman.GetIntArg("-maxmempool")) {
        mempool_opts.max_size_bytes = *mb * 1'000'000;
    }

    if (auto hours = argsman.GetIntArg("-mempoolexpiry")) {
        mempool_opts.expiry = std::chrono::hours{*hours};
    }

    if (argsman.IsArgSet("-minrelaytxfee")) {
        Amount n = Amount::zero();
        auto parsed = ParseMoney(argsman.GetArg("-minrelaytxfee", ""), n);
        if (!parsed || n == Amount::zero()) {
            return AmountErrMsg("minrelaytxfee",
                                argsman.GetArg("-minrelaytxfee", ""));
        }
        // High fee check is done afterward in CWallet::Create()
        mempool_opts.min_relay_feerate = CFeeRate(n);
    }

    mempool_opts.require_standard =
        !argsman.GetBoolArg("-acceptnonstdtxn", !chainparams.RequireStandard());
    if (!chainparams.IsTestChain() && !mempool_opts.require_standard) {
        return strprintf(
            Untranslated(
                "acceptnonstdtxn is not currently supported for %s chain"),
            chainparams.NetworkIDString());
    }

    return std::nullopt;
}
