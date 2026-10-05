// ============================================================================
//  Telegram setup helper: prints your CHAT_ID and sends a test message.
//  1) Create a bot with @BotFather, copy the token into .env (TELEGRAM_TOKEN=...)
//  2) Send "hello" to your bot from Telegram
//  3) Run: npm run telegram
// ============================================================================
import 'dotenv/config';

const TOKEN = process.env.TELEGRAM_TOKEN;
if (!TOKEN) {
  console.log('❌ TELEGRAM_TOKEN missing from the .env file (see the instructions at the top of this file).');
  process.exit(1);
}
const r = await fetch(`https://api.telegram.org/bot${TOKEN}/getUpdates`);
const data = await r.json();
if (!data.ok) { console.log('❌ Token rejected by Telegram:', data.description); process.exit(1); }
const chats = [...new Map(data.result
  .map((u) => u.message?.chat).filter(Boolean)
  .map((c) => [c.id, c])).values()];
if (!chats.length) {
  console.log('⚠️  No message received. Send "hello" to your bot in Telegram, then run this command again.');
  process.exit(0);
}
for (const c of chats) console.log(`✅ CHAT_ID found: ${c.id}  (${c.first_name ?? c.title ?? ''})`);
const chatId = process.env.TELEGRAM_CHAT_ID ?? chats[0].id;
await fetch(`https://api.telegram.org/bot${TOKEN}/sendMessage`, {
  method: 'POST', headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify({ chat_id: chatId, text: '✅ Machine Monitor is connected to Telegram!' }),
});
console.log(`\n→ Add to backend/.env:  TELEGRAM_CHAT_ID=${chatId}`);
console.log('→ A test message has just been sent to your Telegram.');
