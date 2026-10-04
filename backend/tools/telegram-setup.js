// ============================================================================
//  Aide à la configuration Telegram : affiche ton CHAT_ID et envoie un test.
//  1) Crée un bot avec @BotFather, copie le token dans .env (TELEGRAM_TOKEN=...)
//  2) Envoie "bonjour" à ton bot depuis Telegram
//  3) Lance : npm run telegram
// ============================================================================
import 'dotenv/config';

const TOKEN = process.env.TELEGRAM_TOKEN;
if (!TOKEN) {
  console.log('❌ TELEGRAM_TOKEN absent du fichier .env (voir les instructions en haut de ce fichier).');
  process.exit(1);
}
const r = await fetch(`https://api.telegram.org/bot${TOKEN}/getUpdates`);
const data = await r.json();
if (!data.ok) { console.log('❌ Token refusé par Telegram :', data.description); process.exit(1); }
const chats = [...new Map(data.result
  .map((u) => u.message?.chat).filter(Boolean)
  .map((c) => [c.id, c])).values()];
if (!chats.length) {
  console.log('⚠️  Aucun message reçu. Envoie "bonjour" à ton bot dans Telegram, puis relance cette commande.');
  process.exit(0);
}
for (const c of chats) console.log(`✅ CHAT_ID trouvé : ${c.id}  (${c.first_name ?? c.title ?? ''})`);
const chatId = process.env.TELEGRAM_CHAT_ID ?? chats[0].id;
await fetch(`https://api.telegram.org/bot${TOKEN}/sendMessage`, {
  method: 'POST', headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify({ chat_id: chatId, text: '✅ Machine Monitor est connecté à Telegram !' }),
});
console.log(`\n→ Ajoute dans backend/.env :  TELEGRAM_CHAT_ID=${chatId}`);
console.log('→ Un message de test vient d\'être envoyé sur ton Telegram.');
