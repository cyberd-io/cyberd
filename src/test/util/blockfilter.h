// Copyright (c) 2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_UTIL_BLOCKFILTER_H
#define BITCOIN_TEST_UTIL_BLOCKFILTER_H

#include <blockfilter.h>

namespace node {
class BlockManager;
}

bool ComputeFilter(BlockFilterType filter_type, const CBlockIndex &block_index,
                   BlockFilter &filter, const node::BlockManager &blockman);

#endif // BITCOIN_TEST_UTIL_BLOCKFILTER_H
