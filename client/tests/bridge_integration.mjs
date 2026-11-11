import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import net from 'node:net';
import { once } from 'node:events';
import { WebSocket } from 'ws';
const reserve = async () => { const s=net.createServer(); s.listen(0,'127.0.0.1'); await once(s,'listening'); const port=s.address().port; await new Promise(r=>s.close(r)); return port; };
const tcpPort=await reserve(), wsPort=await reserve(), udpPort=await reserve();
const env={...process.env,FASTBID_TCP_PORT:String(tcpPort),FASTBID_WS_PORT:String(wsPort),FASTBID_UDP_MCAST_PORT:String(udpPort)};
const children=[], sockets=[];
const start=(cmd,args)=>{ const p=spawn(cmd,args,{env,stdio:['ignore','pipe','pipe']}); children.push(p); p.stderr.on('data',d=>process.stderr.write(d)); return p; };
const waitUntil=async fn=>{ const deadline=Date.now()+10000; while(!fn()){assert(Date.now()<deadline,'Timed out'); await new Promise(r=>setTimeout(r,20));} };
try {
    const engine=start(process.argv[2],[]); let ready=false;
    engine.stdout.on('data',d=>{ if(d.toString().includes('[Main] Ready'))ready=true; });
    await waitUntil(()=>ready);
    const bridge=start(process.execPath,['ws_bridge.js']); let listening=false;
    bridge.stdout.on('data',d=>{if(d.toString().includes('[Bridge] Listening'))listening=true;});
    await waitUntil(()=>listening);
    async function client(){const ws=new WebSocket(`ws://127.0.0.1:${wsPort}`,{origin:'http://localhost:5173'});sockets.push(ws);ws.messages=[];ws.on('message',d=>ws.messages.push(JSON.parse(d)));await once(ws,'open');await waitUntil(()=>ws.messages.some(m=>m.type==='ready'));return ws;}
    const a=await client(),b=await client();
    a.send(JSON.stringify({type:'new_order',side:'BUY',price:10.00001,quantity:1}));
    await waitUntil(()=>a.messages.some(m=>m.type==='error'));
    a.send(JSON.stringify({type:'new_order',side:'BUY',price:10,quantity:7}));
    await waitUntil(()=>a.messages.some(m=>m.status==='ACK'));
    const ack=a.messages.find(m=>m.status==='ACK'); assert.equal(typeof ack.order_id,'string');
    await waitUntil(()=>a.messages.some(m=>m.type==='book_snapshot'&&m.bids.some(l=>l.price===10&&l.qty===7)));
    b.send(JSON.stringify({type:'cancel',order_id:ack.order_id}));
    await waitUntil(()=>b.messages.some(m=>m.status==='REJECT'&&m.reason===7));
    b.send(JSON.stringify({type:'new_order',side:'SELL',price:10,quantity:7}));
    await waitUntil(()=>a.messages.some(m=>m.status==='FILL')&&b.messages.some(m=>m.status==='FILL'));
    await waitUntil(()=>a.messages.some(m=>m.type==='trade'));
    await new Promise(r=>setTimeout(r,200));
    assert.equal(a.messages.filter(m=>m.type==='trade').length,1,'Trade must not be broadcast twice');
    assert.equal(a.messages.filter(m=>m.status==='ACK').length,1,'Reports must stay on their owner connection');
    assert(!a.messages.some(m=>m.type==='latency_stats'),'No synthetic stats stream');
    // Same prices on different products must not cross. Explicitly test wrong-product cancel.
    a.messages=[]; b.messages=[];
    a.send(JSON.stringify({type:'new_order',instrument_id:2,side:'BUY',price:12,quantity:8}));
    await waitUntil(()=>a.messages.some(m=>m.status==='ACK'));
    const banana=a.messages.find(m=>m.status==='ACK'); assert.equal(banana.instrument_id,2);
    b.send(JSON.stringify({type:'new_order',instrument_id:3,side:'SELL',price:12,quantity:8}));
    await waitUntil(()=>b.messages.some(m=>m.status==='ACK'));
    const orange=b.messages.find(m=>m.status==='ACK');
    assert.notEqual(banana.order_id,orange.order_id);
    await waitUntil(()=>a.messages.some(m=>m.type==='book_snapshot'&&m.instrument_id===2&&m.bids.some(l=>l.price===12&&l.qty===8)));
    await waitUntil(()=>b.messages.some(m=>m.type==='book_snapshot'&&m.instrument_id===3&&m.asks.some(l=>l.price===12&&l.qty===8)));
    assert(!a.messages.some(m=>m.status==='FILL')); assert(!b.messages.some(m=>m.status==='FILL'));
    a.send(JSON.stringify({type:'cancel',instrument_id:3,order_id:banana.order_id}));
    await waitUntil(()=>a.messages.some(m=>m.status==='REJECT'&&m.reason===3));
    b.send(JSON.stringify({type:'new_order',instrument_id:2,side:'SELL',price:12,quantity:3}));
    await waitUntil(()=>a.messages.some(m=>m.status==='FILL'&&m.instrument_id===2&&m.remaining===5));
    a.send(JSON.stringify({type:'cancel',instrument_id:2,order_id:banana.order_id}));
    b.send(JSON.stringify({type:'cancel',instrument_id:3,order_id:orange.order_id}));
    await waitUntil(()=>a.messages.some(m=>m.status==='CANCEL_ACK'&&m.instrument_id===2));
    await waitUntil(()=>b.messages.some(m=>m.status==='CANCEL_ACK'&&m.instrument_id===3));
    const rejected=new WebSocket(`ws://127.0.0.1:${wsPort}`,{origin:'https://untrusted.invalid'});sockets.push(rejected);
    await once(rejected,'error');
    console.log('Bridge integration passed: origin gate, real depth, validated orders, private reports, ownership, single trade broadcast');
} finally {
    for(const s of sockets)s.terminate();
    for(const p of children.reverse()){ if(p.exitCode===null){p.kill('SIGTERM');await Promise.race([once(p,'exit'),new Promise(r=>setTimeout(()=>{p.kill('SIGKILL');r();},5000).unref())]);} }
}
