const { initializeApp } = require('firebase/app');
const { getDatabase, ref, onValue } = require('firebase/database');

const firebaseConfig = {
  databaseURL: "https://ellacloudai-default-rtdb.firebaseio.com",
};

try {
    const app = initializeApp(firebaseConfig);
    const db = getDatabase(app);
    console.log("Firebase initialized successfully");
    process.exit(0);
} catch (e) {
    console.error("Firebase initialization failed:", e.message);
    process.exit(1);
}
