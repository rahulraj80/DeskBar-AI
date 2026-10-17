<?php
require_once '/var/www/conf/auth.php';
$admin = auth_is_admin();

require_once '/var/www/conf/db_connect.php';

header('Cache-Control: no-store');

$GRAPH_COLS = ['CAPTURE_TIME', 'BATT_PCT', 'DRAM_USED_PCT', 'PSRAM_USED_PCT'];

/** Graphable = exists in user_tables and carries all four series columns. */
function find_graph_table($conn, $name, array $cols)
{
    if (!preg_match('/^[A-Za-z][A-Za-z0-9_$#]{0,29}$/', (string)$name)) {
        return null;
    }
    $name = strtoupper($name);
    $in = "'" . implode("', '", $cols) . "'";
    list(, $rows) = db_fetch(
        $conn,
        "SELECT t.table_name
           FROM user_tables t
           JOIN user_tab_columns c ON c.table_name = t.table_name
          WHERE t.table_name = :t
            AND c.column_name IN ({$in})
          GROUP BY t.table_name
         HAVING COUNT(DISTINCT c.column_name) = " . count($cols),
        [':t' => $name]
    );
    return $rows ? $rows[0]['TABLE_NAME'] : null;
}

function list_graph_tables($conn, array $cols)
{
    $in = "'" . implode("', '", $cols) . "'";
    list(, $rows) = db_fetch(
        $conn,
        "SELECT table_name
           FROM user_tab_columns
          WHERE column_name IN ({$in})
          GROUP BY table_name
         HAVING COUNT(DISTINCT column_name) = " . count($cols) . "
          ORDER BY table_name"
    );
    return array_column($rows, 'TABLE_NAME');
}

$raw = trim((string)($_GET['table'] ?? ($_GET['device'] ?? '')));
$candidates = [];
if ($raw !== '') {
    $candidates = [$raw];
    if (!str_starts_with(strtoupper($raw), 'TB_')) {
        $candidates[] = 'TB_' . $raw;
    }
    if (!$admin) {
        $candidates = array_values(array_filter(
            $candidates,
            function ($c) { return str_starts_with(strtoupper($c), 'TB_'); }
        ));
    }
}

$table = null;
try {
    $conn = db_connect();
    foreach ($candidates as $cand) {
        $table = find_graph_table($conn, $cand, $GRAPH_COLS);
        if ($table !== null) {
            break;
        }
    }
    if ($table !== null && !$admin && str_starts_with(strtoupper($table), 'TB_')) {
        require_once __DIR__ . '/device_auth_helper.php';
        enforce_device_totp_auth($conn, $table);
    }
    if ($table === null) {
        if (!$admin) {
            http_response_code(404);
            echo '<!DOCTYPE html><html><head><meta charset="utf-8"><title>Private Device Access</title>'
                . '<style>body{font-family:-apple-system,"Segoe UI",Roboto,sans-serif;margin:2rem;background:#0b0f19;color:#e2e8f0;}'
                . '.box{background:#131b2e;border:1px solid #1e293b;border-radius:10px;padding:1.5rem;max-width:640px;}'
                . 'h2{color:#00d2ff;margin-top:0;}code{background:#0f172a;padding:2px 6px;border-radius:4px;color:#00ff88;}'
                . 'a{color:#00d2ff;}</style></head><body><div class="box">'
                . '<h2>&#128274; Private Device Access Required</h2>'
                . '<p>Telemetry channels are segregated per device to preserve user privacy.</p>'
                . '<p>Graph your device with its specific link from your TuneBar Web Remote or USB Serial CLI:</p>'
                . '<code>https://autobots.my.to/graph.php?table=TB_YOUR_DEVICE_HASH</code></div></body></html>';
            exit;
        }
        $avail = implode(', ', list_graph_tables($conn, $GRAPH_COLS)) ?: '(none)';
        die('<h2>No graphable table found</h2><p>A graphable table needs columns '
            . htmlspecialchars(implode(', ', $GRAPH_COLS)) . '. Available: '
            . htmlspecialchars($avail) . '.</p>'
            . '<p><a href="table.php">&larr; browse all tables (admin)</a></p>');
    }
    define('GRAPH_TABLE', $table);
    list(, $tcols) = db_fetch(
        $conn,
        'SELECT column_name FROM user_tab_columns WHERE table_name = :t',
        [':t' => GRAPH_TABLE]
    );
    $tcol_names = array_column($tcols, 'COLUMN_NAME');

    /** Optional absolute-value metrics for the secondary axis; each needs a different scale. */
    $metric_cols = ['UPTIME_S', 'DRAM_FREE_KB', 'BATT_MV', 'RSSI', 'PSRAM_FREE_KB', 'AUDIO', 'SCREEN'];
    $avail_metrics = [];
    $parts = ['capture_time', 'batt_pct', 'dram_used_pct', 'psram_used_pct'];
    foreach ($metric_cols as $mc) {
        if (in_array($mc, $tcol_names, true)) {
            $parts[] = strtolower($mc);
            $avail_metrics[] = strtolower($mc);
        } else {
            $parts[] = 'NULL AS ' . strtolower($mc);
        }
    }

    /** Find all 5-second survey _CNT columns to scale by 60 into percentage (0-100%) on primary axis */
    $cnt_cols = [];
    foreach ($tcol_names as $col) {
        $cUpper = strtoupper($col);
        if (str_ends_with($cUpper, '_CNT')) {
            $cnt_cols[] = $cUpper;
            $parts[] = strtolower($cUpper);
        }
    }

    list(, $rows) = db_fetch(
        $conn,
        'SELECT ' . implode(', ', $parts) . '
           FROM ' . GRAPH_TABLE . '
          ORDER BY capture_time, ROWID'
    );
} catch (RuntimeException $ex) {
    die('<h2>Could not read ' . htmlspecialchars((string)$table) . '</h2><pre>' . htmlspecialchars($ex->getMessage()) . '</pre>');
}

$points = [];
foreach ($rows as $r) {
    $t = (string)($r['CAPTURE_TIME'] ?? '');
    if (!preg_match('/^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$/', $t)) {
        continue;
    }
    $vals = [];
    // Primary standard metrics (idx 0, 1, 2)
    foreach (['BATT_PCT', 'DRAM_USED_PCT', 'PSRAM_USED_PCT'] as $col) {
        $v = $r[$col] ?? null;
        $vals[] = ($v !== null && is_numeric($v)) ? (float)$v : null;
    }
    // Secondary metrics (idx 3..9)
    foreach ($metric_cols as $col) {
        $v = $r[$col] ?? null;
        $vals[] = ($v !== null && is_numeric($v)) ? (float)$v : null;
    }
    // Survey _CNT columns scaled by 60 into percentage (idx 10..N)
    foreach ($cnt_cols as $col) {
        $v = $r[$col] ?? null;
        if ($v !== null && is_numeric($v)) {
            $pct = round(((float)$v / 60.0) * 100.0, 1);
            if ($pct > 100.0) $pct = 100.0;
            $vals[] = $pct;
        } else {
            $vals[] = null;
        }
    }
    if ($vals[0] === null && $vals[1] === null && $vals[2] === null) {
        continue;
    }
    $points[] = array_merge([$t], $vals);
}

$skipped = count($rows) - count($points);
$data_json = json_encode($points);
$latest = $points ? $points[count($points) - 1][0] : null;
$metrics_json = json_encode($avail_metrics);
$cnt_cols_json = json_encode($cnt_cols);
$is_tb = str_starts_with(GRAPH_TABLE, 'TB_');
$raw_page = $is_tb ? 'telemetry.php' : 'table.php';
?>
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <title>Tunebar Graph - <?php echo htmlspecialchars(GRAPH_TABLE); ?></title>
    <style>
        body { font-family: -apple-system, "Segoe UI", Roboto, sans-serif; margin: 1.5rem; color: #1a1a1a; background: #fafafa; }
        h1 { font-size: 1.4rem; margin-bottom: 0.25rem; }
        .sub { color: #666; font-size: 0.85rem; margin-bottom: 1rem; }
        a { color: #0b57d0; text-decoration: none; }
        a:hover { text-decoration: underline; }
        code { background: #f4f5f7; padding: 1px 4px; border-radius: 3px; }
        .bar { display: flex; align-items: center; gap: 1rem; margin: 0.5rem 0 0.75rem; flex-wrap: wrap; font-size: 0.85rem; }
        .ranges { display: inline-flex; gap: 0; border: 1px solid #d0d0d0; border-radius: 4px; overflow: hidden; }
        .ranges button { border: none; border-right: 1px solid #d0d0d0; background: #fafafa; padding: 3px 10px; cursor: pointer; }
        .ranges button:last-child { border-right: none; }
        .ranges button:hover { background: #f0f0f0; }
        .ranges button.active { background: #0b57d0; color: #fff; }
        select, label { font-size: 0.85rem; }
        .hint { color: #888; font-size: 0.8rem; }
        .chartbox { position: relative; height: 460px; border: 1px solid #d0d0d0; border-radius: 6px; padding: 8px; background: #fff; box-shadow: 0 1px 3px rgba(0,0,0,0.05); }
        .empty { padding: 2rem; color: #666; text-align: center; }
        .btn-sm { font-size:0.85rem; padding:3px 8px; border:1px solid #d0d0d0; border-radius:4px; background:#fff; cursor:pointer; }
        .btn-sm:hover { background:#f5f5f5; }
    </style>
    <script src="assets/chart.umd.js"></script>
    <script src="assets/hammer.min.js"></script>
    <script src="assets/chartjs-plugin-zoom.min.js"></script>
</head>
<body>

<h1>Tunebar device graph (<?php echo htmlspecialchars(GRAPH_TABLE); ?>)</h1>
<p class="sub">
    from <a href="<?php echo $raw_page; ?>?table=<?php echo htmlspecialchars(GRAPH_TABLE); ?>"><?php echo $raw_page; ?>?table=<?php echo htmlspecialchars(GRAPH_TABLE); ?></a>
    <?php if ($latest !== null): ?>
        &middot; <?php echo count($points); ?> points
        &middot; latest capture <?php echo htmlspecialchars((string)$latest); ?>
    <?php endif; ?>
    <?php if ($skipped > 0): ?>
        &middot; <?php echo $skipped; ?> malformed row<?php echo $skipped === 1 ? '' : 's'; ?> skipped
    <?php endif; ?>
    &middot; <a href="<?php echo $raw_page; ?>?table=<?php echo htmlspecialchars(GRAPH_TABLE); ?>">view raw table &rarr;</a>
    <?php if ($admin): ?>
        &middot; <a href="table.php">all tables (admin)</a>
        &middot; <a href="logout.php">log out</a>
    <?php endif; ?>
</p>

<div class="bar">
    <span class="ranges" id="ranges">
        <button data-ms="3600000">1h</button>
        <button data-ms="14400000">4h</button>
        <button data-ms="28800000" class="active">8h</button>
        <button data-ms="86400000">24h</button>
        <button data-ms="604800000">7d</button>
        <button data-ms="all">all</button>
    </span>
    <label>Secondary axis
        <select id="secondary">
            <option value="none">(none)</option>
        </select>
    </label>
    <button id="reset" class="btn-sm">reset zoom</button>
    <span class="hint">drag to pan &middot; pinch/wheel to zoom &middot; click legend items to toggle series (survey % off by default)</span>
</div>

<div class="chartbox">
    <?php if (!$points): ?>
        <div class="empty">No data points with well-formed timestamps found in <code><?php echo htmlspecialchars(GRAPH_TABLE); ?></code>.</div>
    <?php else: ?>
        <canvas id="chart"></canvas>
    <?php endif; ?>
</div>

<script>
const RAW = <?php echo $data_json; ?>;
const AVAILABLE = <?php echo $metrics_json; ?>;
const CNT_COLS = <?php echo $cnt_cols_json; ?>;
const GRAPH_TABLE = <?php echo json_encode(GRAPH_TABLE); ?>;
const IS_TB = <?php echo $is_tb ? 'true' : 'false'; ?>;

const DATA = RAW.map(p => {
    const m = p[0].match(/(\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2}):(\d{2})/);
    return [new Date(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6]).getTime()].concat(p.slice(1));
});
const DEFAULT_MS = 28800000;

const METRICS = {
    uptime_s:      { col: 3, label: 'Uptime',     title: 'Uptime',         human: true,  stepped: false },
    dram_free_kb:  { col: 4, label: 'DRAM free',  title: 'DRAM free (KB)', human: false, stepped: false },
    batt_mv:       { col: 5, label: 'Battery V',  title: 'Battery (V)',   human: false, stepped: false, mul: 0.003 },
    rssi:          { col: 6, label: 'RSSI',       title: 'RSSI (dBm)',     human: false, stepped: false },
    psram_free_kb: { col: 7, label: 'PSRAM free', title: 'PSRAM free (KB)',human: false, stepped: false },
    audio:         { col: 8, label: 'Audio',      title: 'Audio',          human: false, stepped: true },
    screen:        { col: 9, label: 'Screen',     title: 'Screen',         human: false, stepped: true }
};

// Formatted display names and distinct color palette for survey _CNT metrics
function formatCntName(col) {
    const map = {
        'CPU_240M_CNT': 'CPU 240M %',
        'CPU_160M_CNT': 'CPU 160M %',
        'CPU_80M_CNT':  'CPU 80M %',
        'CPU_LOW_CNT':  'CPU Low %',
        'BL_ON_CNT':    'BL On %',
        'BL_DARK_CNT':  'BL Dark %',
        'BL_BOOST_CNT': 'BL Boost %',
        'DISP_30HZ_CNT':'Disp 30Hz %',
        'DISP_ACTIVE_CNT': 'Disp Active %',
        'AUDIO_ACTIVE_CNT': 'Audio Stream %',
        'AUDIO_AMP_CNT':'Audio Amp %',
        'MIC_ACTIVE_CNT':'Mic Rec %',
        'WIFI_CONN_CNT':'Wi-Fi Conn %',
        'WIFI_OFF_CNT': 'Wi-Fi Off %',
        'IMU_ACTIVE_CNT':'IMU Active %',
        'IMU_SLEEP_CNT':'IMU Sleep %',
        'USER_ACTIVE_CNT':'User Active %',
        'BREATHING_CNT':'Breathing %'
    };
    if (map[col]) return map[col];
    return col.replace(/_CNT$/, '').replace(/_/g, ' ') + ' %';
}

const CNT_PALETTE = [
    '#f59e0b', '#ec4899', '#8b5cf6', '#06b6d4', '#14b8a6',
    '#84cc16', '#e11d48', '#3b82f6', '#6366f1', '#d97706',
    '#059669', '#7c3aed', '#db2777', '#ea580c', '#64748b'
];

if (DATA.length) {
    const pad = n => String(n).padStart(2, '0');

    function fmtTick(v, axis) {
        const d = new Date(v);
        const hm = pad(d.getHours()) + ':' + pad(d.getMinutes());
        return (d.getMonth() + 1) + '/' + d.getDate() + ' ' + hm;
    }

    function humanDur(v) {
        if (v >= 172800) return (v / 86400).toFixed(1) + 'd';
        if (v >= 7200) return (v / 3600).toFixed(1) + 'h';
        if (v >= 120) return Math.round(v / 60) + 'm';
        return v + 's';
    }

    function series(idx) {
        return DATA.map(p => ({ x: p[0], y: p[idx + 1] }));
    }

    const line = (label, data, color, axis) => ({
        label: label,
        data: data,
        borderColor: color,
        backgroundColor: color,
        yAxisID: axis,
        pointRadius: 0,
        pointHitRadius: 6,
        borderWidth: 1.5,
        tension: 0.25,
        spanGaps: true
    });

    // Vertical ticks plugin
    const verticalTicks = {
        id: 'verticalTicks',
        afterDraw(chart) {
            const x = chart.scales.x;
            if (!x || !x.ticks.length) return;
            const ctx = chart.ctx;
            ctx.save();
            ctx.font = Chart.helpers.toFont(x.options.ticks.font || {}).string;
            ctx.fillStyle = '#666';
            ctx.textAlign = 'left';
            ctx.textBaseline = 'middle';
            for (const tick of x.ticks) {
                const px = x.getPixelForValue(tick.value);
                if (px < chart.chartArea.left - 20 || px > chart.chartArea.right + 20) continue;
                ctx.translate(px, chart.height - 6);
                ctx.rotate(-Math.PI / 2);
                ctx.fillText(tick.label, 0, 0);
                ctx.rotate(Math.PI / 2);
                ctx.translate(-px, -(chart.height - 6));
            }
            ctx.restore();
        }
    };

    // Primary initial datasets
    const initialDatasets = [
        line('Battery %', series(0), '#10b981', 'y'),
        line('DRAM used %', series(1), '#00d2ff', 'y'),
        line('PSRAM used %', series(2), '#a855f7', 'y'),
        Object.assign(line('(secondary)', [], '#e8710a', 'y2'), { hidden: true, borderDash: [6, 3] })
    ];

    // Append _CNT datasets scaled by 60 into percentage on primary Y axis (OFF by default)
    CNT_COLS.forEach((col, idx) => {
        const seriesIdx = 10 + idx; // 0..2 primary, 3..9 secondary, 10..N cnt
        const color = CNT_PALETTE[idx % CNT_PALETTE.length];
        const ds = Object.assign(line(formatCntName(col), series(seriesIdx), color, 'y'), {
            hidden: true, // Off by default!
            borderDash: [3, 2]
        });
        initialDatasets.push(ds);
    });

    let curMetric = null;
    const chart = new Chart(document.getElementById('chart'), {
        type: 'line',
        plugins: [verticalTicks],
        data: {
            datasets: initialDatasets
        },
        options: {
            parsing: false,
            normalized: true,
            animation: false,
            maintainAspectRatio: false,
            layout: { padding: { bottom: 84 } },
            interaction: { mode: 'nearest', axis: 'x', intersect: false },
            scales: {
                x: {
                    type: 'linear',
                    ticks: {
                        callback: fmtTick,
                        autoSkip: true,
                        maxTicksLimit: 12,
                        display: false
                    },
                    grid: { drawOnChartArea: false }
                },
                y: {
                    position: 'left',
                    min: 0,
                    max: 100,
                    title: { display: true, text: 'Percentage (%)' },
                    grid: { color: '#ececec' }
                },
                y2: {
                    position: 'right',
                    display: false,
                    title: { display: true, text: 'Metric' },
                    grid: { drawOnChartArea: false },
                    ticks: {
                        callback: v => (curMetric && METRICS[curMetric] && METRICS[curMetric].human)
                            ? humanDur(v)
                            : Number(v).toLocaleString('en-US')
                    }
                }
            },
            plugins: {
                legend: {
                    position: 'top',
                    labels: {
                        // Filter out the secondary placeholder from legend
                        filter: item => item.datasetIndex !== 3,
                        boxWidth: 14,
                        padding: 10,
                        font: { size: 11 }
                    },
                    onClick: function(e, legendItem, legend) {
                        const index = legendItem.datasetIndex;
                        const ci = legend.chart;
                        if (ci.isDatasetVisible(index)) {
                            ci.hide(index);
                            legendItem.hidden = true;
                        } else {
                            ci.show(index);
                            legendItem.hidden = false;
                        }
                        savePreferences();
                    }
                },
                tooltip: {
                    callbacks: {
                        title: items => {
                            const d = new Date(items[0].parsed.x);
                            return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate()) +
                                   ' ' + pad(d.getHours()) + ':' + pad(d.getMinutes()) + ':' + pad(d.getSeconds());
                        },
                        label: item => item.dataset.label + ': ' + item.parsed.y + (item.dataset.yAxisID === 'y' ? '%' : '')
                    }
                },
                zoom: {
                    pan: { enabled: true, mode: 'x' },
                    zoom: { wheel: { enabled: true }, pinch: { enabled: true }, mode: 'x' },
                    limits: {
                        x: { min: 'original', max: 'original', minRange: 60 * 1000 }
                    }
                }
            }
        }
    });

    // Populate Secondary Axis dropdown options
    const secondary = document.getElementById('secondary');
    AVAILABLE.forEach(key => {
        const opt = document.createElement('option');
        opt.value = key;
        opt.textContent = METRICS[key].label;
        secondary.appendChild(opt);
    });

    function setWindow(ms, btn, persist = true) {
        const s = chart.options.scales.x;
        if (ms === null) {
            delete s.min;
            delete s.max;
        } else {
            const tmax = DATA[DATA.length - 1][0];
            s.min = tmax - ms;
            s.max = tmax;
        }
        chart.update();
        document.querySelectorAll('#ranges button').forEach(b => b.classList.toggle('active', b === btn));
        if (persist) savePreferences();
    }

    document.querySelectorAll('#ranges button').forEach(b => {
        b.addEventListener('click', () => setWindow(b.dataset.ms === 'all' ? null : Number(b.dataset.ms), b, true));
    });

    function applySecondary(persist = true) {
        curMetric = secondary.value === 'none' ? null : secondary.value;
        const ds = chart.data.datasets[3];
        if (curMetric === null) {
            ds.hidden = true;
            chart.options.scales.y2.display = false;
        } else {
            const m = METRICS[curMetric];
            ds.data = series(m.col);
            if (m.mul) {
                ds.data = ds.data.map(p => ({ x: p.x, y: p.y === null ? null : p.y * m.mul }));
            }
            ds.label = m.label;
            ds.stepped = m.stepped ? 'after' : false;
            ds.hidden = false;
            const y2 = chart.options.scales.y2;
            delete y2.min;
            delete y2.max;
            y2.display = true;
            y2.title.text = m.title;
        }
        chart.update();
        if (persist) savePreferences();
    }
    secondary.addEventListener('change', () => applySecondary(true));

    document.getElementById('reset').addEventListener('click', () => {
        setWindow(DEFAULT_MS, document.querySelector('#ranges button[data-ms="' + DEFAULT_MS + '"]'), true);
    });

    // -------------------------------------------------------------
    // LocalStorage State Persistence
    // -------------------------------------------------------------
    const PREF_KEY = 'tunebar_graph_prefs_' + GRAPH_TABLE;

    function savePreferences() {
        const activeBtn = document.querySelector('#ranges button.active');
        const rangeMs = activeBtn ? activeBtn.dataset.ms : 'all';

        const visibility = {};
        chart.data.datasets.forEach(ds => {
            if (ds.label !== '(secondary)') {
                visibility[ds.label] = chart.isDatasetVisible(chart.data.datasets.indexOf(ds));
            }
        });

        const prefs = {
            range_ms: rangeMs,
            secondary: secondary.value,
            visibility: visibility
        };
        try {
            localStorage.setItem(PREF_KEY, JSON.stringify(prefs));
        } catch (e) {}
    }

    function loadPreferences() {
        let saved = null;
        try {
            const raw = localStorage.getItem(PREF_KEY);
            if (raw) saved = JSON.parse(raw);
        } catch (e) {}

        // 1. Secondary metric preference: default to 'batt_mv' if on a TB_ table and no prior selection
        if (saved && saved.secondary && secondary.querySelector(`option[value="${saved.secondary}"]`)) {
            secondary.value = saved.secondary;
        } else if (IS_TB && AVAILABLE.includes('batt_mv')) {
            secondary.value = 'batt_mv';
        }
        applySecondary(false);

        // 2. Series visibility
        if (saved && saved.visibility) {
            chart.data.datasets.forEach((ds, idx) => {
                if (ds.label !== '(secondary)' && saved.visibility.hasOwnProperty(ds.label)) {
                    const isVis = saved.visibility[ds.label];
                    const meta = chart.getDatasetMeta(idx);
                    meta.hidden = !isVis;
                }
            });
        }

        // 3. Time range window
        let targetMs = DEFAULT_MS;
        let targetBtn = document.querySelector('#ranges button[data-ms="' + DEFAULT_MS + '"]');
        if (saved && saved.range_ms) {
            targetMs = saved.range_ms === 'all' ? null : Number(saved.range_ms);
            targetBtn = document.querySelector(`#ranges button[data-ms="${saved.range_ms}"]`);
        }
        setWindow(targetMs, targetBtn, false);
    }

    // Initialize graph with stored user preferences or smart TB_ defaults
    loadPreferences();
}
</script>
</body>
</html>
