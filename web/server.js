require('dotenv').config();
const express = require('express');
const http = require('http');
const { WebSocketServer } = require('ws');
const mqtt = require('mqtt');
const path = require('path');

const PORT = process.env.PORT || 3000;

const app = express();
app.use(express.json());
app.use(express.text({ type: 'text/plain' }));
app.use(express.static(__dirname));

// Connect to MQTT broker using credentials from .env (never exposed to browser)
const mqttClient = mqtt.connect(process.env.MQTT_URL, {
    username: process.env.MQTT_USERNAME,
    password: process.env.MQTT_PASSWORD,
    clientId: 'ras-backend-' + Math.random().toString(16).slice(2),
    rejectUnauthorized: false
});

// Cache last known device status so new WebSocket clients get it immediately
let lastStatus = null;

mqttClient.on('connect', () => {
    console.log('MQTT Connected');
    mqttClient.subscribe('ras/status', (err) => { if (err) console.error('Subscribe failed:', err); });
    mqttClient.subscribe('ras/schedule/list', (err) => { if (err) console.error('Subscribe failed:', err); });
});

mqttClient.on('error', (err) => {
    console.error('MQTT Error:', err);
});


// REST endpoints — receive from frontend, forward to MQTT
app.post('/schedule/add', (req, res) => {
    mqttClient.publish('ras/schedule/add', JSON.stringify(req.body));
    res.sendStatus(200);
});

app.post('/schedule/delete', (req, res) => {
    mqttClient.publish('ras/schedule/delete', req.body);
    res.sendStatus(200);
});

app.get('/schedule/get', (req, res) => {
    mqttClient.publish('ras/schedule/get', '');
    res.sendStatus(200);
});


// HTTP + WebSocket server
const server = http.createServer(app);
const wss = new WebSocketServer({ server });

const wsClients = new Set();

wss.on('connection', (ws) => {
    wsClients.add(ws);
    console.log('WebSocket client connected, total:', wsClients.size);

    // Send cached status immediately so the UI doesn't stay in "Checking status..."
    if (lastStatus !== null) {
        ws.send(JSON.stringify({ topic: 'ras/status', data: lastStatus }));
    }

    ws.on('close', () => {
        wsClients.delete(ws);
        console.log('WebSocket client disconnected, total:', wsClients.size);
    });
});

// Forward incoming MQTT messages to all connected WebSocket clients
mqttClient.on('message', (topic, message) => {
    const data = message.toString();
    if (topic === 'ras/status') {
        lastStatus = data;
    }
    const payload = JSON.stringify({ topic, data });
    for (const ws of wsClients) {
        if (ws.readyState === ws.OPEN) {
            ws.send(payload);
        }
    }
});

server.listen(PORT, () => {
    console.log(`Server running on http://localhost:${PORT}`);
});
