import { test } from 'node:test';
import assert from 'node:assert/strict';
import { products, validateCatalog } from '../catalog.mjs';

test('catalog has distinct products and integer price bands', () => {
    assert.equal(products.length, 3);
    assert.equal(new Set(products.map(p => p.id)).size, 3);
    assert.deepEqual(products.map(p => p.name), ['Apples', 'Bananas', 'Oranges']);
    assert.equal(products[0].min_price, 5);
});
test('invalid, duplicate and oversized catalogs fail instead of silently routing', () => {
    const p = products[0];
    for (const data of [{}, { products: [] }, { products: [p, p] }, { products: Array(17).fill(p) },
        ...[{ id: 0 }, { id: 1.5 }, { name: '' }, { symbol: '' }, { min_price_ticks: 0 },
            { max_price_ticks: 999999 }, { reference_price_ticks: 1 }].map(patch => ({ products: [{ ...p, ...patch }] }))])
        assert.throws(() => validateCatalog(data));
});
