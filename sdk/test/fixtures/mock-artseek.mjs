import fs from 'node:fs';
import readline from 'node:readline';

// Optional state file path for crashOnce testing across respawns
const stateFile = process.argv[2] || '';

const rl = readline.createInterface({
  input: process.stdin,
  crlfDelay: Infinity,
});

function writeLine(obj, crlf = false) {
  process.stdout.write(`${JSON.stringify(obj)}${crlf ? '\r\n' : '\n'}`);
}

rl.on('line', (line) => {
  const trimmed = line.trim();
  if (!trimmed) return;
  let req;
  try {
    req = JSON.parse(trimmed);
  } catch (err) {
    process.stderr.write(`invalid json: ${err.message}\n`);
    return;
  }

  const params = req.params ?? {};

  // Deliberately emit stderr log before responding
  if (params.emitStderr) {
    process.stderr.write(`${params.emitStderr}\n`);
  }

  // Deliberately emit a stray non-JSON stdout warning line before JSON response
  if (params.emitBanner) {
    process.stdout.write(`WARNING: mock adb banner\r\n`);
  }

  // Simulate mid-request crash once (persisted via stateFile) to test auto-reconnect
  if (params.crashOnce && stateFile) {
    if (!fs.existsSync(stateFile)) {
      fs.writeFileSync(stateFile, String(process.pid), 'utf8');
      process.stderr.write('fatal: simulated segfault on first attempt\n');
      process.exit(139);
    }
  }

  // Simulate permanent crash
  if (params.crashAlways) {
    process.stderr.write('fatal: permanent crash\n');
    process.exit(2);
  }

  // Simulate delay (for timeout or out-of-order correlation testing)
  const delayMs = Number(params.delayMs ?? 0);
  const respond = () => {
    if (params.returnError) {
      writeLine(
        {
          id: req.id,
          ok: false,
          meta: { pid: process.pid, elapsedMs: delayMs },
          data: null,
          error: { code: params.returnError, message: `simulated ${params.returnError}` },
        },
        Boolean(params.crlf),
      );
      return;
    }

    writeLine(
      {
        id: req.id,
        ok: true,
        meta: { pid: process.pid, elapsedMs: delayMs, cacheHit: true },
        data: {
          serverPid: process.pid,
          echoOp: req.op,
          echoParams: params,
          echoPkg: req.pkg ?? null,
        },
        error: null,
      },
      Boolean(params.crlf),
    );
  };

  if (delayMs > 0) {
    setTimeout(respond, delayMs);
  } else {
    respond();
  }
});
