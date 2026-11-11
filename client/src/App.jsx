import React, { useState, useEffect, useRef, useReducer } from 'react';

// --- State Management ---
// We use useReducer to manage the complex state of the order book and trades.
const initialState = {
  products: [],
  groupsReady: false,
  groupPending: false,
  selectedProduct: 1,
  books: {},
  bids: [],
  asks: [],
  trades: [],
  orders: [],
  stats: { ops: 0, p50: 0, p99: 0 },
  connected: false,
  stale: true,
  history: [],
  report: "",
};

function reducer(state, action) {
  switch (action.type) {
    case 'CATALOG': {
      const products = action.payload.products;
      if (products.some(p => p.id === state.selectedProduct))
        return { ...state, products, groupsReady: action.payload.groups_ready === true };
      const selectedProduct = products[0]?.id;
      const snap = state.books[selectedProduct];
      return { ...state, products, groupsReady: action.payload.groups_ready === true,
        selectedProduct, bids: snap?.bids || [], asks: snap?.asks || [],
        stats: snap?.stats || initialState.stats, stale: !snap || Date.now() - snap.at > 2000, history: [] };
    }
    case 'GROUP_PENDING':
      return { ...state, groupPending: true, report: 'Waiting for the engine to register the basket...' };
    case 'GROUP_CREATED': {
      const product = action.payload;
      const products = state.products.some(p => p.id === product.id) ? state.products : [...state.products, product];
      const snap = state.books[product.id];
      return { ...state, products, selectedProduct: product.id, groupPending: false,
        bids: snap?.bids || [], asks: snap?.asks || [], stale: !snap || Date.now() - snap.at > 2000,
        stats: snap?.stats || initialState.stats, history: [], report: `Basket ${product.name} is ready to trade.` };
    }
    case 'SELECT_PRODUCT': {
      const snap = state.books[action.payload];
      return { ...state, selectedProduct: action.payload, bids: snap?.bids || [], asks: snap?.asks || [],
        stats: snap?.stats || initialState.stats, stale: !snap || Date.now() - snap.at > 2000, history: [] };
    }
    case 'CONNECT':
      return { ...state, connected: true };
    case 'DISCONNECT':
      return { ...state, connected: false, stale: true, groupsReady: false, groupPending: false, bids: [], asks: [], books: {}, orders: [], report: "Session disconnected. Earlier resting orders may remain; restart the paper stack for a clean book." };
    case 'STALE':
      if (action.payload !== state.selectedProduct) return { ...state, books: { ...state.books, [action.payload]: undefined } };
      return { ...state, stale: true, books: { ...state.books, [action.payload]: undefined } };
    case 'REPORT': {
      const m = action.payload;
      const name = state.products.find(p => p.id === m.instrument_id)?.name || 'Unknown product';
      return { ...state, groupPending: m.type === 'error' ? false : state.groupPending,
        report: m.message || `${m.status} ${name} order ${m.order_id}${m.reason ? ` (reason ${m.reason})` : ''}` };
    }
    case 'EXECUTION': {
      const m = action.payload;
      let orders = state.orders;
      if (m.status === 'ACK') orders = [...orders, m];
      if (m.status === 'FILL') orders = orders.map(o => o.order_id === m.order_id ? { ...o, remaining: m.remaining } : o).filter(o => o.remaining > 0);
      if (m.status === 'CANCEL_ACK') orders = orders.filter(o => o.order_id !== m.order_id);
      return { ...state, orders };
    }
    case 'BOOK_SNAPSHOT': {
      const books = { ...state.books, [action.payload.instrument_id]: { ...action.payload, at: Date.now() } };
      if (action.payload.instrument_id !== state.selectedProduct) return { ...state, books };
      return {
        ...state, 
        bids: action.payload.bids || [], 
        asks: action.payload.asks || [],
        stale: false,
        stats: action.payload.stats || state.stats,
        history: [...state.history, action.payload.stats?.p99 || 0].slice(-100),
        books
      };
    }
    case 'TRADE':
      // Keep last 50 trades
      const counts = {};
      const newTrades = [action.payload, ...state.trades].filter(t => {
        counts[t.instrument_id] = (counts[t.instrument_id] || 0) + 1;
        return counts[t.instrument_id] <= 50;
      });
      return { ...state, trades: newTrades };
    case 'STATS':
      return { ...state, stats: action.payload };
    default:
      return state;
  }
}

// --- App Component ---
function App() {
  const [state, dispatch] = useReducer(reducer, initialState);
  const [orderForm, setOrderForm] = useState({ price: 10, qty: 100 });
  const [groupName, setGroupName] = useState('');
  const [groupLegs, setGroupLegs] = useState([
    { instrument_id: 1, quantity: 2 }, { instrument_id: 2, quantity: 3 }
  ]);
  const wsRef = useRef(null);
  const product = state.products.find(p => p.id === state.selectedProduct);
  const baseProducts = state.products.filter(p => p.kind !== 'basket' && p.id < 0x80000000);
  const isBasket = product?.kind === 'basket';
  const recipe = (basket, units = 1) => (basket?.legs || []).map(leg =>
    `${leg.quantity * units} × ${state.products.find(p => p.id === leg.instrument_id)?.name || `Product ${leg.instrument_id}`}`).join(' + ');
  const visibleTrades = state.trades.filter(t => t.instrument_id === state.selectedProduct);
  const visibleOrders = state.orders.filter(o => o.instrument_id === state.selectedProduct);

  useEffect(() => {
    const base = state.products.filter(p => p.kind !== 'basket' && p.id < 0x80000000);
    if (!base.length) return;
    setGroupLegs(current => current.every(leg => base.some(p => p.id === leg.instrument_id)) ? current :
      base.slice(0, 2).map((p, i) => ({ instrument_id: p.id, quantity: i + 2 })));
  }, [state.products]);

  const selectProduct = id => {
    const selected = state.products.find(p => p.id === id);
    dispatch({ type: 'SELECT_PRODUCT', payload: id });
    if (selected) setOrderForm({ price: selected.reference_price_ticks / 10000, qty: selected.kind === 'basket' ? 1 : 100 });
  };

  const updateLeg = (index, field, value) => setGroupLegs(legs => legs.map((leg, i) =>
    i === index ? { ...leg, [field]: Number(value) } : leg));

  const createGroup = () => {
    const name = groupName.trim();
    const fail = message => dispatch({ type: 'REPORT', payload: { type: 'error', message } });
    if (!/^[\x20-\x7e]{1,48}$/.test(name)) return fail('Basket name must contain 1-48 printable ASCII characters.');
    if (groupLegs.length < 2 || groupLegs.length > 16) return fail('Choose 2-16 different products for a basket.');
    const ids = new Set();
    for (const leg of groupLegs) {
      if (!baseProducts.some(p => p.id === leg.instrument_id) || ids.has(leg.instrument_id))
        return fail('Each basket row must use a different base product.');
      if (!Number.isInteger(leg.quantity) || leg.quantity < 1 || leg.quantity > 1000)
        return fail('Shares per product must be a whole number from 1 to 1000.');
      ids.add(leg.instrument_id);
    }
    if (groupLegs.reduce((total, leg) => total + leg.quantity, 0) > 10000)
      return fail('A basket can contain at most 10000 shares in total.');
    if (!state.connected || !state.groupsReady || wsRef.current?.readyState !== 1)
      return fail('Basket registration is not ready. Wait for the engine connection.');
    dispatch({ type: 'GROUP_PENDING' });
    wsRef.current.send(JSON.stringify({ type: 'create_group', name, legs: groupLegs }));
  };

  const submitOrder = (side) => {
    if (wsRef.current && wsRef.current.readyState === 1) {
      wsRef.current.send(JSON.stringify({
        type: 'new_order',
        instrument_id: state.selectedProduct,
        side: side,
        price: Number(orderForm.price),
        quantity: Number(orderForm.qty)
      }));
    }
  };

  useEffect(() => {
    let stopped = false;
    let reconnect;
    const connect = () => {
    // Attempt connection
    const wsUrl = import.meta.env.VITE_WS_URL || (import.meta.env.DEV ? 'ws://localhost:8080' : `${location.protocol === 'https:' ? 'wss:' : 'ws:'}//${location.host}`);
    console.log("Connecting to WebSocket:", wsUrl);
    const ws = new WebSocket(wsUrl);
    wsRef.current = ws;

    ws.onopen = () => {}; // readiness requires a live engine TCP session
    ws.onclose = () => { dispatch({ type: 'DISCONNECT' }); if (!stopped) reconnect = setTimeout(connect, 2000); };
    
    ws.onmessage = (event) => {
      try {
        const msg = JSON.parse(event.data);
        if (msg.type === 'catalog') {
          dispatch({ type: 'CATALOG', payload: msg });
        } else if (msg.type === 'group_created') {
          dispatch({ type: 'GROUP_CREATED', payload: msg.product });
          setOrderForm({ price: msg.product.reference_price_ticks / 10000, qty: 1 });
          setGroupName('');
        } else if (msg.type === 'ready') {
          dispatch({ type: 'CONNECT' });
        } else if (msg.type === 'stale') {
          dispatch({ type: 'STALE', payload: msg.instrument_id });
        } else if (msg.type === 'error' || msg.type === 'execution_report') {
          if (msg.type === 'execution_report') dispatch({ type: 'EXECUTION', payload: msg });
          dispatch({ type: 'REPORT', payload: msg });
        } else if (msg.type === 'book_snapshot') {
          dispatch({ type: 'BOOK_SNAPSHOT', payload: msg });
        } else if (msg.type === 'trade') {
          dispatch({ type: 'TRADE', payload: msg });
        } else if (msg.type === 'latency_stats') {
          dispatch({ type: 'STATS', payload: msg });
        }
      } catch (e) {
        console.error("Failed to parse WS message", e);
      }
    };

    };
    connect();
    return () => {
      stopped = true; clearTimeout(reconnect); wsRef.current?.close();
    };
  }, []);

  return (
    <div className="dashboard">
      <header className="header">
        <h1>FastBid Paper Trading</h1>
        <div className={`status ${state.connected ? 'connected' : 'disconnected'}`}>
          {!state.connected ? 'DISCONNECTED' : state.stale ? 'STALE DATA' : 'LIVE'}
        </div>
      </header>

      <p>Simulated orders only. Open a second tab to trade against another session. Restart the stack to reset the book.</p>
      <div className="card product-picker">
        <label htmlFor="product">Product</label>
        <select id="product" value={state.selectedProduct} disabled={!state.products.length}
          onChange={event => selectProduct(Number(event.target.value))}>
          {state.products.map(p => <option key={p.id} value={p.id}>{p.name} ({p.kind === 'basket' ? 'Basket' : p.symbol})</option>)}
        </select>
        <p>Orders match only within {product?.name || 'the selected product'}. Prices are per {isBasket ? 'whole basket' : 'unit'}.</p>
        {isBasket && <div className="basket-recipe">
          <p>One basket: {recipe(product)}.</p>
          <p>Basket units trade together in their own order book. They do not place separate orders in the product books.</p>
        </div>}
      </div>
      <div className="card basket-builder">
        <h2>Create a Product Basket</h2>
        <p>Choose how many shares of each product belong in one basket. The same recipe uses the same tradable basket, even if submitted with another name.</p>
        <div className="form-group">
          <label htmlFor="basket-name">Basket name</label>
          <input id="basket-name" maxLength="48" value={groupName} placeholder="For example, Fruit Mix"
            disabled={state.groupPending} onChange={event => setGroupName(event.target.value)} />
        </div>
        {groupLegs.map((leg, i) => <div className="basket-leg" key={i}>
          <div className="form-group">
            <label htmlFor={`basket-product-${i}`}>Basket product {i + 1}</label>
            <select id={`basket-product-${i}`} value={leg.instrument_id} disabled={state.groupPending}
              onChange={event => updateLeg(i, 'instrument_id', event.target.value)}>
              {baseProducts.map(p => <option key={p.id} value={p.id}>{p.name}</option>)}
            </select>
          </div>
          <div className="form-group">
            <label htmlFor={`basket-shares-${i}`}>Shares in product {i + 1}</label>
            <input id={`basket-shares-${i}`} type="number" min="1" max="1000" step="1" value={leg.quantity}
              disabled={state.groupPending} onChange={event => updateLeg(i, 'quantity', event.target.value)} />
          </div>
          <button type="button" disabled={state.groupPending || groupLegs.length <= 2}
            aria-label={`Remove basket product ${i + 1}`} onClick={() => setGroupLegs(legs => legs.filter((_, index) => index !== i))}>Remove</button>
        </div>)}
        <div className="basket-builder-actions">
          <button type="button" disabled={state.groupPending || groupLegs.length >= Math.min(16, baseProducts.length)} onClick={() => {
            const next = baseProducts.find(p => !groupLegs.some(leg => leg.instrument_id === p.id));
            if (next) setGroupLegs(legs => [...legs, { instrument_id: next.id, quantity: 1 }]);
          }}>Add product</button>
          <button type="button" className="btn-buy" disabled={!state.connected || !state.groupsReady || state.groupPending || baseProducts.length < 2}
            onClick={createGroup}>{state.groupPending ? 'Creating basket...' : 'Create basket'}</button>
        </div>
        {!state.groupsReady && <p>Waiting for basket registration to become available.</p>}
        {state.report && <p className="basket-status" aria-live="polite">{state.report}</p>}
      </div>
      <div className="grid">
        {/* Left Column: Stats and Trades */}
        <div className="left-panel">
          <div className="card stats-card">
            <h2>Engine Request Latency</h2>
            <p>Engine-wide statistics across all products.</p>
            <p>Measured from engine dequeue through matching; excludes network ingress.</p>
            <div className="stat-row">
              <span>Throughput</span>
              <span className="accent-blue">{state.stats.ops.toLocaleString()} ops/s</span>
            </div>
            <div className="stat-row">
              <span>Median (p50)</span>
              <span className="accent-blue">{state.stats.samples ? state.stats.p50 : '—'} ns</span>
            </div>
            <div className="stat-row">
              <span>Tail (p99)</span>
              <span className="accent-blue">{state.stats.samples ? state.stats.p99 : '—'} ns</span>
            </div>
            <svg viewBox="0 0 300 80" role="img" aria-label="Recent p99 engine latency in nanoseconds">
              <polyline fill="none" stroke="#60a5fa" strokeWidth="2" points={state.history.map((n, i) => `${i * 3},${75 - n / Math.max(1, ...state.history) * 70}`).join(" ")} />
            </svg>
            <p>Diagnostic samples dropped: {state.stats.dropped || 0}</p>
          </div>

          <div className="card trades-card">
            <h2>Recent Trades - {product?.name}</h2>
            <div className="trade-list">
              {visibleTrades.length === 0 ? <p className="empty-state">No trades yet</p> : null}
              {visibleTrades.map((t, idx) => (
                <div key={idx} className={`trade-row ${t.aggressor_side === 'BUY' ? 'buy' : 'sell'}`}>
                  <span>{t.price}</span>
                  <span>{t.quantity}{isBasket ? ' basket(s)' : ''}</span>
                </div>
              ))}
            </div>
          </div>
        </div>

        {/* Right Column: Order Book */}
        <div className="card order-book-card">
          <h2>Limit Order Book (Top 10) - {product?.name}</h2>
          {state.stale && <p role="status">Waiting for a fresh engine snapshot</p>}
          
          <div className="book-headers">
            <span>Price</span>
            <span>Quantity</span>
          </div>

          <div className="asks">
            {state.asks.slice().reverse().map((ask, idx) => (
              <div key={`ask-${idx}`} className="book-row ask-row">
                <div className="depth-bar ask-bar" style={{ width: `${Math.min(100, (ask.qty / 1000) * 100)}%` }}></div>
                <span className="price sell">{ask.price}</span>
                <span className="qty">{ask.qty}</span>
              </div>
            ))}
          </div>

          <div className="spread-divider">
            SPREAD: {state.asks.length && state.bids.length ? (state.asks[0].price - state.bids[0].price) : '-'}
          </div>

          <div className="bids">
            {state.bids.map((bid, idx) => (
              <div key={`bid-${idx}`} className="book-row bid-row">
                <div className="depth-bar bid-bar" style={{ width: `${Math.min(100, (bid.qty / 1000) * 100)}%` }}></div>
                <span className="price buy">{bid.price}</span>
                <span className="qty">{bid.qty}</span>
              </div>
            ))}
          </div>
        </div>
      </div>

      <div className="card">
        <h2>My Open Orders</h2>
        <p>{product?.name} - {visibleOrders.length} open order(s). Orders in other products remain active.</p>
        {!visibleOrders.length && <p>No open orders in this session</p>}
        {visibleOrders.map(o => <div className="stat-row" key={o.order_id}>
          <span>#{o.order_id} {o.side} {o.remaining} @ ${o.price}</span>
          <button disabled={!state.connected} onClick={() => wsRef.current?.send(JSON.stringify({ type: 'cancel', order_id: o.order_id, instrument_id: o.instrument_id }))}>Cancel</button>
        </div>)}
      </div>
      <div className="card order-entry-card">
        <h2>Interactive Order Entry - {product?.name}</h2>
        <p role="status">{state.report}</p>
        {isBasket && <p>One basket: {recipe(product)}. Price is the total for one basket; quantity counts whole baskets.</p>}
        <div className="grid" style={{ gridTemplateColumns: '1fr 1fr', gap: '15px' }}>
          <div className="form-group">
            <label htmlFor="order-price">Price {isBasket ? 'per basket ' : ''}(${product?.min_price ?? 5}-${product?.max_price ?? 15})</label>
            <input id="order-price" type="number" min={product?.min_price} max={product?.max_price} step={(product?.tick_size_ticks || 1) / 10000} value={orderForm.price} onChange={e => setOrderForm({...orderForm, price: e.target.value})} />
            {isBasket && <p className="field-help">Price increment: ${(product.tick_size_ticks || 1) / 10000}.</p>}
          </div>
          <div className="form-group">
            <label htmlFor="order-quantity">Quantity</label>
            <input id="order-quantity" type="number" min="1" max="4294967295" step="1" aria-describedby="order-quantity-help" value={orderForm.qty} onChange={e => setOrderForm({...orderForm, qty: e.target.value})} />
            <p id="order-quantity-help" className="field-help">{isBasket ? 'Whole basket units. Every fill keeps the listed share counts together.' : 'Whole units of the selected product.'}</p>
          </div>
        </div>
        <div className="action-buttons">
          <button className="btn-buy" disabled={!state.connected || state.stale || !product} onClick={() => submitOrder('BUY')}>SUBMIT BUY ORDER</button>
          <button className="btn-sell" disabled={!state.connected || state.stale || !product} onClick={() => submitOrder('SELL')}>SUBMIT SELL ORDER</button>
        </div>
      </div>
    </div>
  );
}

export default App;
