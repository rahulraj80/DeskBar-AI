<?php
/**
 * device_auth_helper.php - Dynamic TOTP Hardware Authentication for DeskBar AI
 *
 * Implements RFC 6238-compliant Time-Based One-Time Password (TOTP)
 * verification for isolated device tables (TB_...).
 *
 * Uses HMAC-SHA256 with 30-second rollover and +/- 1 step drift tolerance.
 * Successful validation issues a 30-day Secure; HttpOnly; SameSite=Lax cookie.
 */

function verify_auth_cookie($table, $auth_seed) {
    $cname = "tb_auth_" . strtoupper($table);
    if (empty($_COOKIE[$cname])) return false;
    $parts = explode('.', (string)$_COOKIE[$cname]);
    if (count($parts) !== 2) return false;
    list($ts, $sig) = $parts;
    if (time() - (int)$ts > 30 * 86400) return false; // 30-day expiry
    $expected = hash_hmac('sha256', strtoupper($table) . ":" . $ts, $auth_seed);
    return hash_equals($expected, $sig);
}

function set_auth_cookie($table, $auth_seed) {
    $cname = "tb_auth_" . strtoupper($table);
    $ts = time();
    $sig = hash_hmac('sha256', strtoupper($table) . ":" . $ts, $auth_seed);
    $val = $ts . "." . $sig;
    setcookie($cname, $val, [
        'expires'  => time() + (30 * 86400),
        'path'     => '/',
        'secure'   => true,
        'httponly' => true,
        'samesite' => 'Lax'
    ]);
    $_COOKIE[$cname] = $val;
}

function verify_totp_code($table, $auth_seed, $user_code) {
    $clean = preg_replace('/\s+/', '', trim((string)$user_code));
    if (strlen($clean) !== 6 || !ctype_digit($clean)) return false;
    $t_cur = floor(time() / 30);
    $table_upper = strtoupper($table);

    // Test +/- 1 step (current, previous, next 30s window)
    for ($step = -1; $step <= 1; $step++) {
        $t = $t_cur + $step;
        $msg = $table_upper . ":" . $t;
        $hash = hash_hmac('sha256', $msg, $auth_seed, true);
        $offset = ord($hash[31]) & 0x0F;
        $binary = ((ord($hash[$offset]) & 0x7F) << 24)
                | ((ord($hash[$offset + 1]) & 0xFF) << 16)
                | ((ord($hash[$offset + 2]) & 0xFF) << 8)
                | (ord($hash[$offset + 3]) & 0xFF);
        $code = str_pad($binary % 1000000, 6, '0', STR_PAD_LEFT);
        if ($clean === $code) {
            return true;
        }
    }
    return false;
}

function render_totp_challenge($table, $auth_error = null) {
    $esc_table = htmlspecialchars((string)$table, ENT_QUOTES, 'UTF-8');
    $esc_err = $auth_error ? htmlspecialchars((string)$auth_error, ENT_QUOTES, 'UTF-8') : null;
    ?>
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>DeskBar AI &mdash; Device Access Authentication</title>
<style>
  :root {
    --bg-dark: #070B14;
    --card-bg: #0D1527;
    --card-border: #1E293B;
    --cyan-accent: #00F0FF;
    --cyan-glow: rgba(0, 240, 255, 0.15);
    --text-main: #F1F5F9;
    --text-muted: #94A3B8;
    --danger: #EF4444;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
  body {
    background-color: var(--bg-dark);
    color: var(--text-main);
    display: flex;
    align-items: center;
    justify-content: center;
    min-height: 100vh;
    padding: 1.5rem;
  }
  .auth-card {
    background: var(--card-bg);
    border: 1px solid var(--card-border);
    border-radius: 16px;
    max-width: 540px;
    width: 100%;
    padding: 2.2rem;
    box-shadow: 0 20px 40px rgba(0,0,0,0.6);
  }
  .badge-header {
    display: flex;
    align-items: center;
    gap: 12px;
    margin-bottom: 1.5rem;
  }
  .badge-icon {
    font-size: 28px;
  }
  .brand-title {
    font-size: 1.3rem;
    font-weight: 700;
    letter-spacing: 0.5px;
    color: var(--text-main);
  }
  .brand-sub {
    font-size: 0.82rem;
    color: var(--cyan-accent);
    text-transform: uppercase;
    letter-spacing: 1px;
    margin-top: 2px;
  }
  .table-badge {
    background: #131E35;
    border: 1px solid #1E3A8A;
    border-radius: 8px;
    padding: 0.75rem 1rem;
    margin-bottom: 1.5rem;
    display: flex;
    justify-content: space-between;
    align-items: center;
  }
  .table-label { font-size: 0.78rem; color: var(--text-muted); text-transform: uppercase; }
  .table-val { font-family: ui-monospace, SFMono-Regular, Menlo, monospace; font-weight: 600; color: #FBBF24; font-size: 0.95rem; }
  .screen-guide {
    background: #060911;
    border: 1px dashed #334155;
    border-radius: 10px;
    padding: 1rem 1.2rem;
    margin-bottom: 1.5rem;
  }
  .guide-title {
    font-size: 0.82rem;
    color: var(--text-main);
    font-weight: 600;
    margin-bottom: 8px;
    display: flex;
    align-items: center;
    gap: 6px;
  }
  .mockup-display {
    background: #0A0F1D;
    border: 1px solid var(--cyan-accent);
    border-radius: 8px;
    padding: 10px 14px;
    font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-top: 6px;
  }
  .mockup-text { font-size: 0.78rem; color: var(--text-muted); }
  .mockup-code { font-size: 1.15rem; font-weight: 700; color: var(--cyan-accent); letter-spacing: 3px; }
  .mockup-timer { font-size: 0.8rem; color: #FBBF24; }
  .input-label {
    display: block;
    font-size: 0.85rem;
    font-weight: 600;
    margin-bottom: 0.5rem;
    color: var(--text-main);
  }
  .code-input {
    width: 100%;
    background: #070B14;
    border: 2px solid #334155;
    border-radius: 8px;
    padding: 12px;
    font-size: 1.5rem;
    font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
    letter-spacing: 8px;
    text-align: center;
    color: var(--cyan-accent);
    outline: none;
    transition: border-color 0.2s, box-shadow 0.2s;
  }
  .code-input:focus {
    border-color: var(--cyan-accent);
    box-shadow: 0 0 16px var(--cyan-glow);
  }
  .btn-submit {
    width: 100%;
    margin-top: 1.2rem;
    padding: 14px;
    background: linear-gradient(135deg, #0284C7, #0369A1);
    border: none;
    border-radius: 8px;
    color: #fff;
    font-size: 1rem;
    font-weight: 600;
    cursor: pointer;
    transition: opacity 0.2s;
  }
  .btn-submit:hover { opacity: 0.92; }
  .error-alert {
    background: rgba(239, 68, 68, 0.15);
    border: 1px solid var(--danger);
    color: #FCA5A5;
    padding: 10px 14px;
    border-radius: 8px;
    font-size: 0.85rem;
    margin-bottom: 1.2rem;
  }
  .note-footer {
    font-size: 0.75rem;
    color: #64748B;
    text-align: center;
    margin-top: 1.2rem;
    line-height: 1.4;
  }
</style>
</head>
<body>
<div class="auth-card">
  <div class="badge-header">
    <span class="badge-icon">&#128272;</span>
    <div>
      <div class="brand-title">DeskBar AI Authentication</div>
      <div class="brand-sub">Dynamic Hardware Token (TOTP)</div>
    </div>
  </div>

  <div class="table-badge">
    <span class="table-label">Isolated Device Table</span>
    <span class="table-val"><?= $esc_table ?></span>
  </div>

  <?php if (!empty($esc_err)): ?>
    <div class="error-alert">&#9888; <?= $esc_err ?></div>
  <?php endif; ?>

  <div class="screen-guide">
    <div class="guide-title">&#128241; View Live Code on your DeskBar AI Screen:</div>
    <p style="font-size:0.8rem; color:#CBD5E1; margin-bottom:8px;">Open <strong>Utilities &gt; System Info &gt; Cloud &amp; PIN</strong> (or check during boot setup):</p>
    <div class="mockup-display">
      <span class="mockup-text">WEB ACCESS CODE:</span>
      <span class="mockup-code">&#9679;&#9679;&#9679; &#9679;&#9679;&#9679;</span>
      <span class="mockup-timer">&#9201; 24s</span>
    </div>
  </div>

  <form method="POST" autocomplete="off">
    <input type="hidden" name="action" value="verify_totp">
    <label class="input-label" for="totp_code">Enter 6-Digit Live Access Code</label>
    <input class="code-input" id="totp_code" name="totp_code" type="text" maxlength="7" placeholder="000 000" autofocus required>
    <button type="submit" class="btn-submit">Authenticate Device &rarr;</button>
  </form>

  <p class="note-footer">
    &#128737; Protected by hardware-seeded HMAC-SHA256 tokens.<br>
    Successful validation issues a secure 30-day session cookie for this browser.
  </p>
</div>
</body>
</html>
    <?php
}

function enforce_device_totp_auth($conn, $table) {
    if (empty($table) || !str_starts_with(strtoupper($table), 'TB_')) {
        return;
    }
    $table = strtoupper(trim((string)$table));

    $auth_seed = null;
    try {
        list(, $arows) = db_fetch($conn, "SELECT auth_seed FROM tb_device_auth WHERE table_name = :t", [":t" => $table]);
        if ($arows && !empty($arows[0]["AUTH_SEED"])) {
            $auth_seed = trim((string)$arows[0]["AUTH_SEED"]);
        }
    } catch (Exception $e) {
        $auth_seed = null;
    }

    // If device hasn't registered an auth seed yet, allow backward compatibility
    if (empty($auth_seed)) {
        return;
    }

    $auth_error = null;
    if ($_SERVER["REQUEST_METHOD"] === "POST" && isset($_POST["action"]) && $_POST["action"] === "verify_totp") {
        $entered = $_POST["totp_code"] ?? "";
        if (verify_totp_code($table, $auth_seed, $entered)) {
            set_auth_cookie($table, $auth_seed);
            $target = $_SERVER["REQUEST_URI"] ?? ("telemetry.php?table=" . urlencode($table));
            header("Location: " . $target);
            exit;
        } else {
            $auth_error = "Invalid 6-digit access code. Please check the current code on your DeskBar AI display.";
        }
    }

    if (!verify_auth_cookie($table, $auth_seed)) {
        render_totp_challenge($table, $auth_error);
        exit;
    }
}
