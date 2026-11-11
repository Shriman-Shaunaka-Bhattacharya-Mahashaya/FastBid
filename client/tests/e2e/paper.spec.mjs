import { test, expect } from '@playwright/test';

async function ready(page) {
    await page.goto('/');
    await expect(page.getByRole('heading', { name: 'FastBid Paper Trading' })).toBeVisible();
    await expect(page.getByText('LIVE', { exact: true })).toBeVisible();
}
async function submit(page, side, price, qty) {
    await page.getByLabel('Price').fill(String(price));
    await page.getByLabel('Quantity', { exact: true }).fill(String(qty));
    await page.getByRole('button', { name: `SUBMIT ${side} ORDER` }).click();
}
const openOrders = page => page.locator('.card').filter({ has: page.getByRole('heading', { name: 'My Open Orders' }) });

test('page loads real readiness, empty state, diagnostics and labelled inputs', async ({ page, request }) => {
    const errors = []; page.on('pageerror', error => errors.push(error.message));
    await ready(page);
    await expect(openOrders(page).getByText('No open orders in this session')).toBeVisible();
    await expect(page.getByRole('img', { name: 'Recent p99 engine latency in nanoseconds' })).toBeVisible();
    await expect(page.getByLabel('Price')).toHaveValue('10');
    const health = await request.get('/healthz'); expect(health.status()).toBe(200);
    expect(await health.json()).toEqual({ ready: true, mode: 'paper' });
    expect(errors).toEqual([]);
});

test('acknowledged order appears, publishes depth, and cancels', async ({ page }) => {
    await ready(page); await submit(page, 'BUY', 8.0001, 17);
    await expect(openOrders(page)).toContainText('BUY 17 @ $8.0001');
    await expect(page.locator('.bids')).toContainText('8.0001');
    await openOrders(page).getByRole('button', { name: 'Cancel', exact: true }).click();
    await expect(openOrders(page)).toContainText('No open orders');
    await expect(page.getByRole('status').filter({ hasText: 'CANCEL_ACK' })).toBeVisible();
    await expect(page.locator('.bids')).not.toContainText('8.0001');
});

test('two sessions partially fill, retain the remainder, and publish one trade', async ({ page, context }) => {
    const other = await context.newPage(); await ready(page); await ready(other);
    await submit(page, 'BUY', 9.0002, 100);
    await expect(openOrders(page)).toContainText('BUY 100 @ $9.0002');
    await submit(other, 'SELL', 9.0002, 40);
    await expect(openOrders(page)).toContainText('BUY 60 @ $9.0002');
    await expect(openOrders(other)).toContainText('No open orders');
    await expect(other.getByRole('status').filter({ hasText: 'FILL' })).toBeVisible();
    await expect(page.locator('.trade-row')).toHaveCount(1);
    await expect(other.locator('.trade-row')).toHaveCount(1);
    await expect(page.locator('.trade-row')).toContainText('9.0002');
    await openOrders(page).getByRole('button', { name: 'Cancel', exact: true }).click();
    await expect(openOrders(page)).toContainText('No open orders');
    await other.close();
});

test('bad price and quantity show errors without creating orders', async ({ page }) => {
    await ready(page); await submit(page, 'BUY', 16, 1);
    await expect(page.getByRole('status').filter({ hasText: 'Price must be' })).toBeVisible();
    await submit(page, 'SELL', 10, 0);
    await expect(page.getByRole('status').filter({ hasText: 'Invalid side, price or quantity' })).toBeVisible();
    await expect(openOrders(page)).toContainText('No open orders');
});

test('connection loss disables entry and reconnects', async ({ page }) => {
    let disconnected = false, active;
    await page.routeWebSocket('**/*', route => {
        if (disconnected) route.close({ code: 1001, reason: 'Test transport interruption' });
        else { active = route; route.connectToServer(); }
    });
    await ready(page);
    disconnected = true;
    active.close({ code: 1001, reason: 'Test transport interruption' });
    await expect(page.getByRole('button', { name: 'SUBMIT BUY ORDER' })).toBeDisabled();
    disconnected = false;
    await expect(page.getByText('LIVE', { exact: true })).toBeVisible({ timeout: 15000 });
    await expect(page.getByRole('button', { name: 'SUBMIT BUY ORDER' })).toBeEnabled();
});

test('narrow viewport keeps trading controls usable', async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 }); await ready(page);
    await page.getByLabel('Price').scrollIntoViewIfNeeded();
    await expect(page.getByLabel('Price')).toBeVisible();
    await expect(page.getByRole('button', { name: 'SUBMIT SELL ORDER' })).toBeEnabled();
});

test('product switching preserves orders and prevents cross-product matches', async ({ page, context }) => {
    const other = await context.newPage(); await ready(page); await ready(other);
    await expect(page.getByLabel('Product', { exact: true })).toHaveValue('1');
    await submit(page, 'BUY', 11.0003, 20);
    await expect(openOrders(page)).toContainText('BUY 20 @ $11.0003');
    await other.getByLabel('Product', { exact: true }).selectOption('2');
    await submit(other, 'SELL', 11.0003, 20);
    await expect(openOrders(other)).toContainText('SELL 20 @ $11.0003');
    // These prices cross, but different products must both remain resting.
    await expect(openOrders(page)).toContainText('BUY 20 @ $11.0003');
    await expect(page.locator('.trade-row')).toHaveCount(0);
    await page.getByLabel('Product', { exact: true }).selectOption('2');
    await expect(openOrders(page)).toContainText('No open orders');
    await expect(page.locator('.asks')).toContainText('11.0003');
    await submit(page, 'BUY', 11.0003, 7);
    await expect(openOrders(other)).toContainText('SELL 13 @ $11.0003');
    await expect(page.locator('.trade-row')).toHaveCount(1);
    await page.getByLabel('Product', { exact: true }).selectOption('1');
    await expect(openOrders(page)).toContainText('BUY 20 @ $11.0003');
    await expect(page.locator('.trade-row')).toHaveCount(0);
    await openOrders(page).getByRole('button', { name: 'Cancel', exact: true }).click();
    await expect(openOrders(page)).toContainText('No open orders');
    await openOrders(other).getByRole('button', { name: 'Cancel', exact: true }).click();
    await expect(openOrders(other)).toContainText('No open orders');
    await page.getByLabel('Product', { exact: true }).selectOption('3');
    await expect(page.locator('.bids .book-row')).toHaveCount(0);
    await expect(page.locator('.asks .book-row')).toHaveCount(0);
    await other.close();
});

test('basket creation validates its name, distinct products and share counts', async ({ page }) => {
    await ready(page);
    const create = page.getByRole('button', { name: 'Create basket', exact: true });
    await expect(create).toBeEnabled();
    await page.getByLabel('Basket name', { exact: true }).fill('   ');
    await create.click();
    await expect(page.getByRole('status').filter({ hasText: 'Basket name must contain' })).toBeVisible();
    await page.getByLabel('Basket name', { exact: true }).fill('Invalid shares');
    await page.getByLabel('Shares in product 1', { exact: true }).fill('0');
    await create.click();
    await expect(page.getByRole('status').filter({ hasText: 'Shares per product must be' })).toBeVisible();
    await page.getByLabel('Shares in product 1', { exact: true }).fill('2');
    await page.getByLabel('Basket product 2', { exact: true }).selectOption('1');
    await create.click();
    await expect(page.getByRole('status').filter({ hasText: 'Each basket row must use a different base product' })).toBeVisible();
    await expect(page.getByLabel('Product', { exact: true })).toHaveValue('1');
});

test('whole baskets register globally, fill in fixed ratios, and reuse an identical recipe', async ({ page, context }) => {
    const other = await context.newPage(); await ready(page); await ready(other);
    await other.getByLabel('Product', { exact: true }).selectOption('3');
    await page.getByLabel('Basket name', { exact: true }).fill('Fruit Mix');
    await page.getByLabel('Basket product 1', { exact: true }).selectOption('1');
    await page.getByLabel('Shares in product 1', { exact: true }).fill('2');
    await page.getByLabel('Basket product 2', { exact: true }).selectOption('2');
    await page.getByLabel('Shares in product 2', { exact: true }).fill('3');
    const create = page.getByRole('button', { name: 'Create basket', exact: true });
    await expect(create).toBeEnabled(); await create.click();
    await expect.poll(async () => Number(await page.getByLabel('Product', { exact: true }).inputValue())).toBeGreaterThanOrEqual(0x80000000);
    const basketId = await page.getByLabel('Product', { exact: true }).inputValue();
    await expect(page.getByLabel('Basket name', { exact: true })).toHaveValue('');
    await expect(page.getByLabel('Price')).toHaveValue('50');
    await expect(page.locator('.basket-recipe')).toContainText('One basket: 2 × Apples + 3 × Bananas.');
    await expect(other.getByLabel('Product', { exact: true })).toHaveValue('3');
    await expect(other.getByLabel('Product', { exact: true }).locator(`option[value="${basketId}"]`)).toHaveCount(1);
    await other.getByLabel('Product', { exact: true }).selectOption(basketId);
    await expect(page.getByText('LIVE', { exact: true })).toBeVisible();
    await expect(other.getByText('LIVE', { exact: true })).toBeVisible();
    await submit(page, 'BUY', 50, 3);
    await expect(openOrders(page)).toContainText('BUY 3 @ $50');
    await submit(other, 'SELL', 50, 1);
    await expect(openOrders(page)).toContainText('BUY 2 @ $50');
    await expect(openOrders(other)).toContainText('No open orders');
    await expect(page.locator('.trade-row')).toHaveCount(1);
    await expect(page.locator('.trade-row')).toContainText('1 basket(s)');
    await openOrders(page).getByRole('button', { name: 'Cancel', exact: true }).click();
    await expect(openOrders(page)).toContainText('No open orders');
    for (const id of ['1', '2']) {
        await page.getByLabel('Product', { exact: true }).selectOption(id);
        await expect(page.locator('.bids .book-row')).toHaveCount(0);
        await expect(page.locator('.asks .book-row')).toHaveCount(0);
    }
    await other.getByLabel('Basket name', { exact: true }).fill('Same Recipe Alias');
    await other.getByRole('button', { name: 'Create basket', exact: true }).click();
    await expect(other.getByLabel('Basket name', { exact: true })).toHaveValue('');
    await expect(other.getByLabel('Product', { exact: true })).toHaveValue(basketId);
    await expect(page.getByLabel('Product', { exact: true })).toHaveValue('2');
    await expect(other.getByLabel('Product', { exact: true }).locator(`option[value="${basketId}"]`)).toHaveCount(1);
    await other.close();
});
