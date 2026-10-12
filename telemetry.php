<?php
/**
 * telemetry.php - Isolated Per-Device Telemetry Viewer for TuneBar
 * Requires mandatory device table parameter (?table=TB_... or ?device=...)
 * NEVER enumerates other user tables to ensure multi-tenant privacy.
 */

require_once "/var/www/conf/db_connect.php";

try {
    $conn = db_connect();
} catch (RuntimeException $ex) {
    die("<h2>Database unavailable</h2><pre>" . htmlspecialchars($ex->getMessage()) . "</pre>");
}

function esc($v) {
    return htmlspecialchars((string)$v, ENT_QUOTES, "UTF-8");
}

$raw_name = $_GET["table"] ?? ($_GET["device"] ?? "");
if (!empty($raw_name) && !str_starts_with(strtoupper($raw_name), "TB_")) {
    $raw_name = "TB_" . $raw_name;
}

$selected = null;
if (preg_match("/^TB_[A-Za-z0-9_]{4,26}$/i", (string)$raw_name)) {
    $name = strtoupper($raw_name);
    list(, $trows) = db_fetch($conn, "SELECT table_name FROM user_tables WHERE table_name = :t", [":t" => $name]);
    if ($trows) {
        $selected = $trows[0]["TABLE_NAME"];
        $regFile = "/var/www/data/registered_devices.json";
        $devs = file_exists($regFile) ? json_decode(@file_get_contents($regFile), true) : [];
        if (!is_array($devs)) $devs = [];
        if (!isset($devs[$selected])) {
            $devs[$selected] = [
                'first_seen' => date('Y-m-d H:i:s'),
                'last_seen' => date('Y-m-d H:i:s'),
                'ip' => $_SERVER['REMOTE_ADDR'] ?? '-',
                'fw' => 'telemetry',
                'hits' => 1
            ];
            @file_put_contents($regFile, json_encode($devs, JSON_PRETTY_PRINT), LOCK_EX);
        }
    }
}

$page_size = isset($_GET["size"]) ? (int)$_GET["size"] : 25;
if (!in_array($page_size, [10, 25, 50, 100, 250], true)) {
    $page_size = 25;
}
$page = isset($_GET["page"]) ? max(1, (int)$_GET["page"]) : 1;

$columns = [];
$rows = [];
$total = 0;
$err = null;

if ($selected) {
    try {
        list(, $tcount) = db_fetch($conn, "SELECT COUNT(*) as C FROM {$selected}");
        $total = (int)($tcount[0]["C"] ?? 0);

        list(, $cols) = db_fetch($conn,
            "SELECT column_name, data_type FROM user_tab_cols WHERE table_name = :t ORDER BY column_id",
            [":t" => $selected]
        );
        $columns = $cols ?: [];

        $sort_col = "CAPTURE_TIME";
        $col_names = array_column($columns, "COLUMN_NAME");
        if (!in_array($sort_col, $col_names, true)) {
            $sort_col = $col_names[0] ?? "1";
        }
        $sort_dir = "DESC";

        $total_pages = $total > 0 ? (int)ceil($total / $page_size) : 1;
        $page = min($page, $total_pages);
        $offset = ($page - 1) * $page_size;

        list(, $rows) = db_fetch(
            $conn,
            "SELECT * FROM {$selected} ORDER BY {$sort_col} {$sort_dir} OFFSET {$offset} ROWS FETCH NEXT {$page_size} ROWS ONLY"
        );
    } catch (RuntimeException $ex) {
        $err = $ex->getMessage();
    }
}
?>
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <title>TuneBar Device Telemetry</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; margin: 1.5rem; background: #0b0f19; color: #e2e8f0; }
        .card { background: #131b2e; border: 1px solid #1e293b; border-radius: 8px; padding: 1.25rem; margin-bottom: 1.25rem; }
        h1 { font-size: 1.4rem; color: #00d2ff; margin-top: 0; }
        h2 { font-size: 1.1rem; color: #00ff88; margin-top: 0; }
        table { border-collapse: collapse; width: 100%; font-size: 0.85rem; background: #0f172a; border-radius: 6px; overflow: hidden; }
        th, td { border: 1px solid #1e293b; padding: 6px 10px; text-align: left; }
        th { background: #1e293b; color: #00d2ff; font-weight: 600; }
        tr:nth-child(even) { background: #141e33; }
        .badge { display: inline-block; padding: 2px 8px; border-radius: 12px; font-size: 0.75rem; font-weight: bold; }
        .badge-ok { background: #064e3b; color: #34d399; }
        .badge-warn { background: #78350f; color: #fbbf24; }
        .metrics-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap: 1rem; margin-bottom: 1.25rem; }
        .metric-box { background: #1e293b; padding: 0.75rem; border-radius: 6px; border-left: 3px solid #00d2ff; }
        .metric-val { font-size: 1.2rem; font-weight: bold; color: #f8fafc; }
        .metric-lbl { font-size: 0.75rem; color: #94a3b8; }
        a { color: #00d2ff; text-decoration: none; }
        a:hover { text-decoration: underline; }
        .null { color: #64748b; font-style: italic; }
        .err-box { background: #450a0a; border: 1px solid #7f1d1d; color: #fca5a5; padding: 1rem; border-radius: 6px; }
        .info-box { background: #082f49; border: 1px solid #0369a1; color: #bae6fd; padding: 1rem; border-radius: 6px; }
    </style>
</head>
<body>

<h1>TuneBar Private Telemetry Portal</h1>

<?php if (!$selected): ?>
    <div class="err-box">
        <h3>🔒 Private Device Access Required</h3>
        <p>Telemetry channels are segregated per device to preserve user privacy.</p>
        <p>Please access your metrics using your specific device link from your TuneBar Web Remote or USB Serial CLI:</p>
        <code>https://autobots.my.to/telemetry.php?table=TB_YOUR_DEVICE_HASH</code>
    </div>
<?php else: ?>
    <?php
    $latest = $rows[0] ?? null;
    if ($latest):
        $batt_v = isset($latest["BATT_MV"]) ? number_format($latest["BATT_MV"] / 1000.0, 2) . "V" : "-";
        $batt_pct = $latest["BATT_PCT"] ?? "-";
        $rssi = $latest["RSSI"] ?? "-";
        $uptime_m = isset($latest["UPTIME_S"]) ? floor($latest["UPTIME_S"] / 60) . "m" : "-";
    ?>
    <div class="card">
        <h2>Device Channel: <code><?php echo esc($selected); ?></code></h2>
        <div class="metrics-grid">
            <div class="metric-box">
                <div class="metric-lbl">Latest Battery</div>
                <div class="metric-val"><?php echo esc($batt_v); ?> (<?php echo esc($batt_pct); ?>%)</div>
            </div>
            <div class="metric-box" style="border-left-color: #38bdf8;">
                <div class="metric-lbl">Wi-Fi Signal</div>
                <div class="metric-val"><?php echo esc($rssi); ?> dBm</div>
            </div>
            <div class="metric-box" style="border-left-color: #a855f7;">
                <div class="metric-lbl">Uptime</div>
                <div class="metric-val"><?php echo esc($uptime_m); ?></div>
            </div>
            <div class="metric-box" style="border-left-color: #00ff88;">
                <div class="metric-lbl">Logged Records</div>
                <div class="metric-val"><?php echo number_format($total); ?></div>
            </div>
        </div>
    </div>
    <?php endif; ?>

    <div class="card">
        <h3>Telemetry Records (Page <?php echo $page; ?> of <?php echo max(1, (int)ceil($total / $page_size)); ?>)</h3>
        <table>
            <thead>
                <tr>
                <?php foreach ($columns as $c): ?>
                    <th><?php echo esc($c["COLUMN_NAME"]); ?></th>
                <?php endforeach; ?>
                </tr>
            </thead>
            <tbody>
            <?php foreach ($rows as $r): ?>
                <tr>
                <?php foreach ($columns as $c):
                    $col = $c["COLUMN_NAME"];
                    $val = $r[$col] ?? null;
                ?>
                    <td<?php echo $val === null ? ' class="null"' : ''; ?>>
                        <?php echo $val === null ? 'null' : esc($val); ?>
                    </td>
                <?php endforeach; ?>
                </tr>
            <?php endforeach; ?>
            <?php if (!$rows): ?>
                <tr><td colspan="<?php echo count($columns); ?>">No telemetry records logged yet.</td></tr>
            <?php endif; ?>
            </tbody>
        </table>
    </div>
<?php endif; ?>

</body>
</html>
