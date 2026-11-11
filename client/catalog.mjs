import { readFile } from 'node:fs/promises';

export function validateCatalog(data) {
    if (!Array.isArray(data?.products) || data.products.length < 1 || data.products.length > 16)
        throw Error('Catalog requires 1 to 16 products');
    const ids = new Set();
    return data.products.map(p => {
        if (!Number.isInteger(p.id) || p.id < 1 || p.id >= 2147483648 || ids.has(p.id) ||
            typeof p.name !== 'string' || !p.name.trim() || typeof p.symbol !== 'string' || !p.symbol.trim() ||
            ![p.min_price_ticks, p.max_price_ticks, p.reference_price_ticks].every(n => Number.isSafeInteger(n) && n > 0 && n <= 4294967295) ||
            p.max_price_ticks < p.min_price_ticks || p.max_price_ticks - p.min_price_ticks + 1 > 100001 ||
            p.reference_price_ticks < p.min_price_ticks || p.reference_price_ticks > p.max_price_ticks)
            throw Error('Invalid or duplicate catalog product');
        ids.add(p.id);
        return Object.freeze({ ...p, min_price: p.min_price_ticks / 10000, max_price: p.max_price_ticks / 10000 });
    });
}
export const products = validateCatalog(JSON.parse(await readFile(new URL('../config/products.json', import.meta.url), 'utf8')));
