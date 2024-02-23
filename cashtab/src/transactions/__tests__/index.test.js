// Copyright (c) 2023 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
import {
    sendXec,
    getMultisendTargetOutputs,
    ignoreUnspendableUtxos,
} from 'transactions';
import { getSendTokenInputs, getSlpSendTargetOutputs } from 'slpv1';
import { MockChronikClient } from '../../../../apps/mock-chronik-client';
import {
    sendXecVectors,
    getMultisendTargetOutputsVectors,
    ignoreUnspendableUtxosVectors,
    sendSlp,
} from '../fixtures/vectors';

describe('Improved Cashtab transaction broadcasting function', () => {
    // Unit test for each vector in fixtures for the sendingXecToSingleAddress case
    const { txs, errors } = sendXecVectors;

    // Successfully built and broadcast txs
    txs.forEach(async tx => {
        const {
            description,
            wallet,
            targetOutputs,
            feeRate,
            chaintipBlockheight,
            txid,
            hex,
        } = tx;
        it(`sendXec: ${description}`, async () => {
            const chronik = new MockChronikClient();
            chronik.setMock('broadcastTx', {
                input: hex,
                output: { txid },
            });
            expect(
                await sendXec(
                    chronik,
                    wallet,
                    targetOutputs,
                    feeRate,
                    chaintipBlockheight,
                ),
            ).toStrictEqual({ hex, response: { txid } });
        });
    });

    // Error cases
    errors.forEach(async error => {
        const { description, wallet, targetOutputs, feeRate, msg, hex } = error;

        it(`sendXec: ${description}`, async () => {
            const chronik = new MockChronikClient();
            // e.g. ('block', {input: '', output: ''})
            if (typeof hex !== 'undefined') {
                // For error cases that are not thrown until after the tx is successfully built,
                // set a tx broadcast error that can be thrown by the broadcasting eCash node
                chronik.setMock('broadcastTx', {
                    input: hex,
                    output: new Error(msg),
                });
            }

            await expect(
                sendXec(chronik, wallet, targetOutputs, feeRate),
            ).rejects.toThrow(msg);
        });
    });
});

describe('Forming multisend targetOutputs', () => {
    // Unit test for each vector in fixtures for the getMultisendTargetOutputs case
    const { formedOutputs, errors } = getMultisendTargetOutputsVectors;

    // Successfully built and broadcast txs
    formedOutputs.forEach(async formedOutput => {
        const { description, userMultisendInput, targetOutputs } = formedOutput;
        it(`getMultisendTargetOutputs: ${description}`, () => {
            expect(getMultisendTargetOutputs(userMultisendInput)).toStrictEqual(
                targetOutputs,
            );
        });
    });

    // Error cases
    errors.forEach(async error => {
        const { description, userMultisendInput, msg } = error;

        it(`getMultisendTargetOutputs throws error for: ${description}`, () => {
            expect(() => getMultisendTargetOutputs(userMultisendInput)).toThrow(
                msg,
            );
        });
    });
});

describe('Ignore unspendable coinbase utxos', () => {
    // Unit test for each vector in fixtures for the ignoreUnspendableUtxos case
    const { expectedReturns } = ignoreUnspendableUtxosVectors;

    // Successfully built and broadcast txs
    expectedReturns.forEach(async formedOutput => {
        const {
            description,
            unfilteredUtxos,
            chaintipBlockheight,
            spendableUtxos,
        } = formedOutput;
        it(`ignoreUnspendableUtxos: ${description}`, () => {
            expect(
                ignoreUnspendableUtxos(unfilteredUtxos, chaintipBlockheight),
            ).toStrictEqual(spendableUtxos);
        });
    });
});

describe('We can create and broadcast SLP v1 SEND txs from utxos of nng or in-node chronik shape', () => {
    // Unit test for each vector in fixtures for the sendingXecToSingleAddress case
    const { expectedReturnsNng, expectedReturnsInNode } = sendSlp;

    // Successfully builds and broadcasts txs for NNG input utxos
    expectedReturnsNng.forEach(async tx => {
        const {
            description,
            wallet,
            tokenId,
            sendQty,
            sendAmounts,
            tokenInputs,
            destinationAddress,
            feeRate,
            chaintipBlockheight,
            txid,
            hex,
        } = tx;
        it(`Build and broadcast an SLP V1 SEND tx: ${description}`, async () => {
            const chronik = new MockChronikClient();
            chronik.setMock('broadcastTx', {
                input: hex,
                output: { txid },
            });

            // Get tokenInputs and sendAmounts
            const tokenInputInfo = getSendTokenInputs(
                wallet.state.slpUtxos,
                tokenId,
                sendQty,
            );

            expect(tokenInputInfo.tokenInputs).toStrictEqual(tokenInputs);
            expect(tokenInputInfo.sendAmounts).toStrictEqual(sendAmounts);

            // Get the targetOutputs
            const tokenSendTargetOutputs = getSlpSendTargetOutputs(
                tokenInputInfo,
                destinationAddress,
            );

            // Send it
            expect(
                await sendXec(
                    chronik,
                    wallet,
                    tokenSendTargetOutputs,
                    feeRate,
                    chaintipBlockheight,
                    tokenInputInfo.tokenInputs,
                ),
            ).toStrictEqual({ hex, response: { txid } });
        });
    });

    // Successfully builds and broadcasts txs for in-node chronik-client-shaped input utxos
    expectedReturnsInNode.forEach(async tx => {
        const {
            description,
            wallet,
            tokenId,
            sendQty,
            decimals,
            sendAmounts,
            tokenInputs,
            destinationAddress,
            feeRate,
            chaintipBlockheight,
            txid,
            hex,
        } = tx;
        it(`Build and broadcast an SLP V1 SEND tx: ${description}`, async () => {
            const chronik = new MockChronikClient();
            chronik.setMock('broadcastTx', {
                input: hex,
                output: { txid },
            });

            // Get tokenInputs and sendAmounts
            const tokenInputInfo = getSendTokenInputs(
                wallet.state.slpUtxos,
                tokenId,
                sendQty,
                decimals,
            );

            expect(tokenInputInfo.tokenInputs).toStrictEqual(tokenInputs);
            expect(tokenInputInfo.sendAmounts).toStrictEqual(sendAmounts);

            // Get the targetOutputs
            const tokenSendTargetOutputs = getSlpSendTargetOutputs(
                tokenInputInfo,
                destinationAddress,
            );
            // Send it
            expect(
                await sendXec(
                    chronik,
                    wallet,
                    tokenSendTargetOutputs,
                    feeRate,
                    chaintipBlockheight,
                    tokenInputInfo.tokenInputs,
                ),
            ).toStrictEqual({ hex, response: { txid } });
        });
    });
});
