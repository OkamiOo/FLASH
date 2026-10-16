// Pilote un OBS par obs-websocket (protocole v5), pour les essais automatiques.
// Usage : node tools/obs-ws.mjs <config.json> <commande> [arguments]
//   setup <adresse> <port>     ajoute « FLASH vidéo » et « FLASH audio » à la scène courante
//   record-start | record-stop
//   screenshot <source> <fichier.png>
//   stats
import { readFileSync, writeFileSync } from 'node:fs';
import { createHash } from 'node:crypto';

const [configPath, command, ...args] = process.argv.slice(2);
const config = JSON.parse(readFileSync(configPath, 'utf8'));

const sha256b64 = (s) => createHash('sha256').update(s).digest('base64');

function connect() {
	return new Promise((resolve, reject) => {
		const ws = new WebSocket(config.websocket_url, 'obswebsocket.json');
		const pending = new Map();
		let nextId = 1;

		const request = (requestType, requestData = {}) =>
			new Promise((res) => {
				const requestId = String(nextId++);
				pending.set(requestId, res);
				ws.send(JSON.stringify({ op: 6, d: { requestType, requestId, requestData } }));
			});

		ws.onerror = () => reject(new Error('OBS injoignable'));
		ws.onmessage = (event) => {
			const msg = JSON.parse(event.data);
			if (msg.op === 0) {
				const d = { rpcVersion: 1 };
				const auth = msg.d.authentication;
				if (auth) d.authentication = sha256b64(sha256b64(config.websocket_password + auth.salt) + auth.challenge);
				ws.send(JSON.stringify({ op: 1, d }));
			} else if (msg.op === 2) {
				resolve({ ws, request });
			} else if (msg.op === 7) {
				const res = pending.get(msg.d.requestId);
				pending.delete(msg.d.requestId);
				res?.(msg.d);
			}
		};
	});
}

function check(reply, tolerated = []) {
	const status = reply.requestStatus;
	if (!status.result && !tolerated.includes(status.code))
		throw new Error(`${reply.requestType} : ${status.code} ${status.comment ?? ''}`);
	return reply.responseData ?? {};
}

const { ws, request } = await connect();
try {
	if (command === 'setup') {
		const [address = '127.0.0.1', port = '9710'] = args;
		const { currentProgramSceneName: sceneName } = check(await request('GetCurrentProgramScene'));
		const settings = { address, port: Number(port), track: 1 };
		for (const [inputName, inputKind] of [
			['FLASH vidéo', 'flash_video_source'],
			['FLASH audio', 'flash_audio_source'],
		]) {
			// 601 : la source existe déjà
			check(await request('CreateInput', { sceneName, inputName, inputKind, inputSettings: settings }), [601]);
			check(await request('SetInputSettings', { inputName, inputSettings: settings }));
		}
		console.log(`Sources FLASH prêtes dans « ${sceneName} »`);
	} else if (command === 'record-start') {
		check(await request('StartRecord'), [500]);
		console.log('Enregistrement lancé');
	} else if (command === 'record-stop') {
		const { outputPath } = check(await request('StopRecord'));
		console.log(outputPath);
	} else if (command === 'screenshot') {
		const [sourceName, file] = args;
		const { imageData } = check(
			await request('GetSourceScreenshot', { sourceName, imageFormat: 'png', imageWidth: 960 }),
		);
		writeFileSync(file, Buffer.from(imageData.split(',')[1], 'base64'));
		console.log(file);
	} else if (command === 'stats') {
		console.log(JSON.stringify(check(await request('GetStats')), null, 2));
	} else {
		throw new Error(`commande inconnue : ${command}`);
	}
} finally {
	ws.close();
}
