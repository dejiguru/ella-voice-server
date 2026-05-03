const b64c = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
let key = [];
for (let i = 0; i < 16; i++) key.push(Math.floor(Math.random() * 256));
let keyB64 = [];
let i = 0;
while (i < 16) {
    let a = i < 16 ? key[i++] : 0;
    let b = i < 16 ? key[i++] : 0;
    let c = i < 16 ? key[i++] : 0;
    let n = (a << 16) | (b << 8) | c;
    keyB64.push(b64c.charAt((n >> 18) & 63));
    keyB64.push(b64c.charAt((n >> 12) & 63));
    keyB64.push(b64c.charAt((n >> 6) & 63));
    keyB64.push(b64c.charAt(n & 63));
}
keyB64[22] = '=';
keyB64[23] = '=';
console.log(keyB64.join(''), keyB64.length);
console.log(Buffer.from(key).toString('base64'));
