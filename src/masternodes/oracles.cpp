// Copyright (c) 2020 DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <masternodes/oracles.h>

/// @attention make sure that it does not overlap with those in masternodes.cpp/tokens.cpp/undos.cpp/accounts.cpp/orders.cpp !!!
const unsigned char COraclesWeightView::ByOracleWeightId::prefix = 'p';

void COraclesWeightView::ForEachPriceOracleWeight(std::function<bool (const CScript & oracle, const CAmount & weight)> callback, const CScript &start) const
{
    ForEach<ByOracleWeightId, CScript, CAmount>([&callback] (const CScript & oracle, const CAmount & weight) {
        return callback(oracle, weight);
    }, start);
}

boost::optional<CAmount> COraclesWeightView::GetOracleWeight(const CScript &oracle) const
{
    return ReadBy<ByOracleWeightId, CAmount>(oracle);
}

Res COraclesWeightView::SetOracleWeight(const CCreateWeightOracleMessage &oracleMsg) {
    WriteBy<ByOracleWeightId>(oracleMsg.oracle, oracleMsg.weight);
    return Res::Ok();
}

Res COraclesWeightView::DelOracleWeight(CScript const & oracle) {
    EraseBy<ByOracleWeightId>(oracle);
    return Res::Ok();
}
