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

$raw_name = $_GET["table"] ?? ($_GET["device"] ?? ($_POST["table"] ?? ""));
if (!empty($raw_name) && !str_starts_with(strtoupper($raw_name), "TB_")) {
    $raw_name = "TB_" . $raw_name;
}

$selected = null;
if (preg_match("/^TB_[A-Za-z0-9_]{4,26}$/i", (string)$raw_name)) {
    $name = strtoupper($raw_name);
    list(, $trows) = db_fetch($conn, "SELECT table_name FROM user_tables WHERE table_name = :t", [":t" => $name]);
    if ($trows) {
        $selected = $trows[0]["TABLE_NAME"];
        require_once __DIR__ . "/device_auth_helper.php";
        enforce_device_totp_auth($conn, $selected);
    }
}

// -------------------------------------------------------------
// Action 1: Handle Delete Range of Records by LOG_ID (POST)
// -------------------------------------------------------------
$delete_msg = null;
$delete_err = null;
if ($_SERVER["REQUEST_METHOD"] === "POST" && isset($_POST["action"]) && $_POST["action"] === "delete_range" && $selected) {
    $start_id = isset($_POST["start_id"]) ? (int)$_POST["start_id"] : 0;
    $end_id = isset($_POST["end_id"]) ? (int)$_POST["end_id"] : 0;
    if ($start_id > 0 && $end_id > 0) {
        $min_id = min($start_id, $end_id);
        $max_id = max($start_id, $end_id);
        try {
            $affected = db_exec(
                $conn,
                "DELETE FROM {$selected} WHERE LOG_ID >= :min_id AND LOG_ID <= :max_id",
                [":min_id" => $min_id, ":max_id" => $max_id]
            );
            $delete_msg = "Successfully deleted " . number_format($affected) . " record(s) from LOG_ID {$min_id} to {$max_id}.";
        } catch (RuntimeException $ex) {
            $delete_err = "Delete failed: " . $ex->getMessage();
        }
    } else {
        $delete_err = "Please enter valid numeric Start and Finish LOG_IDs.";
    }
}

// -------------------------------------------------------------
// Action 2: Handle CSV Download (GET)
// -------------------------------------------------------------
if (isset($_GET["action"]) && $_GET["action"] === "export_csv" && $selected) {
    $mode = $_GET["mode"] ?? "latest"; // "latest" or "first"
    $count_raw = $_GET["count"] ?? "100";
    $count = ($count_raw === "all") ? 0 : max(1, (int)$count_raw);

    list(, $cols) = db_fetch($conn,
        "SELECT column_name FROM user_tab_cols WHERE table_name = :t ORDER BY column_id",
        [":t" => $selected]
    );
    $col_names = array_column($cols, "COLUMN_NAME");

    $sort_dir = ($mode === "first") ? "ASC" : "DESC";
    $sql = "SELECT * FROM {$selected} ORDER BY LOG_ID {$sort_dir}";
    if ($count > 0) {
        $sql .= " FETCH NEXT {$count} ROWS ONLY";
    }

    list(, $export_rows) = db_fetch($conn, $sql);

    $filename = $selected . "_" . $mode . "_" . ($count > 0 ? $count : "all") . ".csv";

    header("Content-Type: text/csv; charset=utf-8");
    header("Content-Disposition: attachment; filename=\"{$filename}\"");
    header("Pragma: no-cache");
    header("Expires: 0");

    $fp = fopen("php://output", "w");
    // Write UTF-8 BOM so Microsoft Excel opens it cleanly
    fputs($fp, "\xEF\xBB\xBF");
    fputcsv($fp, $col_names);
    foreach ($export_rows as $row) {
        $line = [];
        foreach ($col_names as $col) {
            $line[] = $row[$col] ?? "";
        }
        fputcsv($fp, $line);
    }
    fclose($fp);
    exit;
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
        if (in_array("LOG_ID", $col_names, true)) {
            $sort_col = "LOG_ID";
        } elseif (!in_array($sort_col, $col_names, true)) {
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
        table { border-collapse: collapse; width: 100%; font-size: 0.82rem; background: #0f172a; border-radius: 6px; overflow-x: auto; display: block; white-space: nowrap; }
        th, td { border: 1px solid #1e293b; padding: 6px 10px; text-align: left; }
        th { background: #1e293b; color: #00d2ff; font-weight: 600; position: sticky; top: 0; z-index: 2; }
        tr:nth-child(even) { background: #141e33; }
        tr:hover { background: #1e293b; }
        .metrics-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap: 1rem; margin-bottom: 1.25rem; }
        .metric-box { background: #1e293b; padding: 0.75rem; border-radius: 6px; border-left: 3px solid #00d2ff; }
        .metric-val { font-size: 1.2rem; font-weight: bold; color: #f8fafc; }
        .metric-lbl { font-size: 0.75rem; color: #94a3b8; }
        a { color: #00d2ff; text-decoration: none; }
        a:hover { text-decoration: underline; }
        .null { color: #64748b; font-style: italic; }
        .err-box { background: #450a0a; border: 1px solid #7f1d1d; color: #fca5a5; padding: 1rem; border-radius: 6px; margin-bottom: 1rem; }
        .succ-box { background: #064e3b; border: 1px solid #059669; color: #6ee7b7; padding: 1rem; border-radius: 6px; margin-bottom: 1rem; }
        .action-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 1rem; margin-bottom: 1.25rem; }
        .action-card { background: #0f172a; border: 1px solid #1e293b; border-radius: 6px; padding: 1rem; }
        .action-card h3 { margin-top: 0; font-size: 0.95rem; color: #38bdf8; display: flex; align-items: center; gap: 0.4rem; }
        .btn { display: inline-flex; align-items: center; gap: 0.4rem; padding: 6px 12px; border-radius: 5px; font-size: 0.82rem; font-weight: 600; cursor: pointer; border: none; text-decoration: none; transition: background 0.15s; }
        .btn-primary { background: #0284c7; color: #fff; }
        .btn-primary:hover { background: #0369a1; text-decoration: none; }
        .btn-danger { background: #b91c1c; color: #fff; }
        .btn-danger:hover { background: #991b1b; }
        .form-row { display: flex; gap: 0.5rem; align-items: center; flex-wrap: wrap; margin-top: 0.5rem; }
        .input-text, .select-box { background: #1e293b; border: 1px solid #334155; color: #f8fafc; padding: 5px 8px; border-radius: 4px; font-size: 0.82rem; }
        .input-text:focus, .select-box:focus { border-color: #0284c7; outline: none; }
        .btn-del-sm { background: #450a0a; border: 1px solid #991b1b; color: #fca5a5; padding: 2px 6px; border-radius: 3px; font-size: 0.72rem; cursor: pointer; }
        .btn-del-sm:hover { background: #991b1b; color: #fff; }
    </style>
</head>
<body>

<h1>TuneBar Private Telemetry Portal</h1>

<?php if ($delete_msg): ?>
    <div class="succ-box">
        ✓ <?php echo esc($delete_msg); ?>
    </div>
<?php endif; ?>

<?php if ($delete_err): ?>
    <div class="err-box">
        ⚠️ <?php echo esc($delete_err); ?>
    </div>
<?php endif; ?>

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
        $batt_v = isset($latest["BATT_MV"]) ? number_format($latest["BATT_MV"] * 3.0 / 1000.0, 2) . "V" : "-";
        $batt_pct = $latest["BATT_PCT"] ?? "-";
        $rssi = $latest["RSSI"] ?? "-";
        $uptime_m = isset($latest["UPTIME_S"]) ? floor($latest["UPTIME_S"] / 60) . "m" : "-";
    ?>
    <div class="card">
        <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:0.75rem;flex-wrap:wrap;gap:0.5rem">
            <h2 style="margin:0">Device Channel: <code><?php echo esc($selected); ?></code></h2>
            <a href="graph.php?table=<?php echo urlencode($selected); ?>" style="background:#0d233a;border:1px solid #00d2ff;color:#00d2ff;padding:4px 10px;border-radius:6px;font-size:0.8rem;text-decoration:none;font-weight:bold">📈 View Device Graph &rarr;</a>
        </div>
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

        <!-- Data Management & Export Actions -->
        <div class="action-grid">
            <!-- 1. CSV Download Card -->
            <div class="action-card">
                <h3>📥 Download Telemetry (CSV)</h3>
                <form method="get" action="telemetry.php" style="margin:0">
                    <input type="hidden" name="table" value="<?php echo esc($selected); ?>">
                    <input type="hidden" name="action" value="export_csv">
                    <div class="form-row">
                        <label style="font-size:0.8rem;color:#94a3b8">Fetch:</label>
                        <select name="mode" class="select-box">
                            <option value="latest">Latest</option>
                            <option value="first">First (Earliest)</option>
                        </select>
                        <select name="count" class="select-box">
                            <option value="50">50 records</option>
                            <option value="100" selected>100 records</option>
                            <option value="250">250 records</option>
                            <option value="500">500 records</option>
                            <option value="1000">1000 records</option>
                            <option value="all">All records (<?php echo $total; ?>)</option>
                        </select>
                        <button type="submit" class="btn btn-primary">Download CSV</button>
                    </div>
                </form>
            </div>

            <!-- 2. Delete Range by LOG_ID Card -->
            <div class="action-card" style="border-color:#450a0a">
                <h3 style="color:#f87171">🗑️ Delete Records by LOG_ID</h3>
                <form method="post" action="telemetry.php" style="margin:0" onsubmit="return confirmDeleteRange()">
                    <input type="hidden" name="table" value="<?php echo esc($selected); ?>">
                    <input type="hidden" name="action" value="delete_range">
                    <div class="form-row">
                        <label style="font-size:0.8rem;color:#94a3b8">Start:</label>
                        <input type="number" id="start_id" name="start_id" class="input-text" style="width:90px" placeholder="Start ID" required>
                        <label style="font-size:0.8rem;color:#94a3b8">Finish:</label>
                        <input type="number" id="end_id" name="end_id" class="input-text" style="width:90px" placeholder="Finish ID" required>
                        <button type="submit" class="btn btn-danger">Delete</button>
                    </div>
                    <div style="font-size:0.73rem;color:#94a3b8;margin-top:0.35rem">
                        Tip: Typing Start ID automatically sets Finish ID to delete a single row. Change Finish ID for a multi-row range.
                    </div>
                </form>
            </div>
        </div>
    </div>
    <?php endif; ?>

    <div class="card">
        <h3>Telemetry Records (Page <?php echo $page; ?> of <?php echo max(1, (int)ceil($total / $page_size)); ?>)</h3>
        <table>
            <thead>
                <tr>
                    <th style="min-width:50px">Action</th>
                <?php foreach ($columns as $c): ?>
                    <th><?php echo esc($c["COLUMN_NAME"]); ?></th>
                <?php endforeach; ?>
                </tr>
            </thead>
            <tbody>
            <?php foreach ($rows as $r):
                $lid = $r["LOG_ID"] ?? null;
            ?>
                <tr>
                    <td>
                        <?php if ($lid !== null): ?>
                            <button type="button" class="btn-del-sm" onclick="setDeleteId('<?php echo esc($lid); ?>')" title="Select this LOG_ID to delete">Del #<?php echo esc($lid); ?></button>
                        <?php else: ?>
                            -
                        <?php endif; ?>
                    </td>
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
                <tr><td colspan="<?php echo count($columns) + 1; ?>">No telemetry records logged yet.</td></tr>
            <?php endif; ?>
            </tbody>
        </table>
        <?php
        $total_pages = max(1, (int)ceil($total / $page_size));
        $qs = function ($p) use ($selected, $page_size) {
            return "?table=" . urlencode($selected) . "&size=" . $page_size . "&page=" . $p;
        };
        ?>
        <div style="margin-top:0.75rem;display:flex;gap:0.75rem;align-items:center;flex-wrap:wrap">
            <span>Page <?php echo $page; ?> of <?php echo $total_pages; ?></span>
            <?php if ($page > 1): ?>
                <a href="<?php echo esc($qs(1)); ?>">first</a>
                <a href="<?php echo esc($qs($page - 1)); ?>">prev</a>
            <?php endif; ?>
            <?php if ($page < $total_pages): ?>
                <a href="<?php echo esc($qs($page + 1)); ?>">next</a>
                <a href="<?php echo esc($qs($total_pages)); ?>">last</a>
            <?php endif; ?>
            <form method="get" style="display:inline">
                <input type="hidden" name="table" value="<?php echo esc($selected); ?>">
                <label>Rows
                    <select name="size" class="select-box" onchange="this.form.submit()">
                        <?php foreach ([10, 25, 50, 100, 250] as $s): ?>
                            <option value="<?php echo $s; ?>" <?php echo $s === $page_size ? 'selected' : ''; ?>><?php echo $s; ?></option>
                        <?php endforeach; ?>
                    </select>
                </label>
            </form>
        </div>
    </div>
<?php endif; ?>

<script>
const startInput = document.getElementById('start_id');
const endInput = document.getElementById('end_id');

if (startInput && endInput) {
    startInput.addEventListener('input', function() {
        if (!endInput.dataset.manual || endInput.value === '') {
            endInput.value = startInput.value;
        }
    });

    endInput.addEventListener('input', function() {
        endInput.dataset.manual = 'true';
    });
}

function setDeleteId(id) {
    if (startInput && endInput) {
        startInput.value = id;
        endInput.value = id;
        delete endInput.dataset.manual;
        startInput.scrollIntoView({ behavior: 'smooth', block: 'center' });
        startInput.focus();
    }
}

function confirmDeleteRange() {
    const s = parseInt(startInput.value, 10);
    const e = parseInt(endInput.value, 10);
    if (!s || !e) {
        alert('Please specify both Start and Finish LOG_IDs.');
        return false;
    }
    const minId = Math.min(s, e);
    const maxId = Math.max(s, e);
    const countDesc = (minId === maxId) ? `LOG_ID #${minId}` : `range from LOG_ID #${minId} to #${maxId}`;
    return confirm(`⚠️ Are you sure you want to PERMANENTLY DELETE ${countDesc}?\n\nThis operation cannot be undone.`);
}
</script>

</body>
</html>
