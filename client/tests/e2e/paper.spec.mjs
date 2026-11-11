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
