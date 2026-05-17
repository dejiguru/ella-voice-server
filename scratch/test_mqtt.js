const mqtt = require('mqtt');

const MQTT_URL = 'wss://29a395ae67d44ef5a9803532efe719ce.s1.eu.hivemq.cloud:8884/mqtt';
const MQTT_OPTIONS = {
    username: 'ellarobot',
    password: 'Ella1234',
    protocol: 'wss',
    port: 8884,
    reconnectPeriod: 5000,
    clean: true,
};

console.log('Connecting to HiveMQ Cloud...');
const client = mqtt.connect(MQTT_URL, MQTT_OPTIONS);

client.on('connect', () => {
    console.log('CONNECTED to HiveMQ Cloud successfully!');
    client.subscribe('ella/#', (err) => {
        if (!err) {
            console.log('Subscribed to ella/#');
        } else {
            console.log('Subscription error:', err);
        }
    });
});

client.on('message', (topic, message) => {
    console.log(`Received message on ${topic}: ${message.toString()}`);
});

client.on('error', (err) => {
    console.error('MQTT Error:', err);
});

client.on('close', () => {
    console.log('Connection closed');
});

// Exit after 10 seconds
setTimeout(() => {
    console.log('Closing test...');
    client.end();
    process.exit(0);
}, 10000);
