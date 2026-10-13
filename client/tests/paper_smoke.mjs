import assert from 'node:assert/strict';
import { WebSocket } from 'ws';
const base = process.argv[2] || 'http://localhost:8080';
const health = await fetch(`${base}/healthz`);
assert.equal(health.status,200); assert.equal((await health.json()).mode,'paper');
const page=await fetch(base); assert.equal(page.status,200);
const html=await page.text(); const asset=html.match(/src="([^"]+\.js)"/)[1];
assert.equal((await fetch(new URL(asset,base))).status,200);
assert.equal((await fetch(`${base}/ws_bridge.js`)).status,404);
const clients=[];
const wait=async f=>{const until=Date.now()+5000;while(!f()){assert(Date.now()<until,'Timed out waiting for report');await new Promise(r=>setTimeout(r,20));}};
async function connect(){const ws=new WebSocket(base.replace(/^http/,'ws'),{origin:base});clients.push(ws);ws.messages=[];ws.on('message',d=>ws.messages.push(JSON.parse(d)));await wait(()=>ws.messages.some(m=>m.type==='ready'));return ws;}
try {
 const a=await connect(), b=await connect();
 a.send(JSON.stringify({type:'new_order',side:'BUY',price:9,quantity:3}));
 await wait(()=>a.messages.some(m=>m.status==='ACK'));
 const ack=a.messages.find(m=>m.status==='ACK');assert.equal(ack.side,'BUY');assert.equal(ack.remaining,3);
 a.send(JSON.stringify({type:'cancel',order_id:ack.order_id}));
 await wait(()=>a.messages.some(m=>m.status==='CANCEL_ACK'));
 a.messages=[];
 a.send(JSON.stringify({type:'new_order',side:'BUY',price:10,quantity:7}));
 await wait(()=>a.messages.some(m=>m.status==='ACK'));
 b.send(JSON.stringify({type:'new_order',side:'SELL',price:10,quantity:7}));
 await wait(()=>a.messages.some(m=>m.status==='FILL')&&b.messages.some(m=>m.status==='FILL'));
 await wait(()=>a.messages.some(m=>m.type==='trade'));
 await wait(()=>a.messages.some(m=>m.type==='book_snapshot'&&!m.bids.length&&!m.asks.length));
 console.log('Paper stack passed: HTTP assets, health, restricted assets, ACK details, cancellation, two-session matching, real empty-book snapshot.');
} finally {for(const ws of clients)ws.close();}
