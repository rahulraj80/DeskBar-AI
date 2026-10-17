<?php
require_once '/var/www/conf/auth.php';
auth_require($_SERVER['REQUEST_URI'] ?? '/table.php');

require_once '/var/www/conf/db_connect.php';

header('Cache-Control: no-store');

try {
    $conn = db_connect();
} catch (RuntimeException $ex) {
    die('<h2>Database unavailable</h2><pre>' . htmlspecialchars($ex->getMessage()) . '</pre>');
}

function esc($v)
{
    return htmlspecialchars((string)$v, ENT_QUOTES, 'UTF-8');
}

/** Identifiers cannot be bound, so resolve them against the catalogue instead. */
function find_table($conn, $name)
{
    if (!preg_match('/^[A-Za-z][A-Za-z0-9_$#]{0,29}$/', (string)$name)) {
        return null;
    }
    $name = strtoupper($name);
    list(, $rows) = db_fetch($conn, 'SELECT table_name FROM user_tables WHERE table_name = :t', [':t' => $name]);
    return $rows ? $rows[0]['TABLE_NAME'] : null;
}

/** Delete the first/last N rows of a table relative to the current sort order. */
if ($_SERVER['REQUEST_METHOD'] === 'POST' && isset($_POST['do_prune'])) {
    $back = [];
    foreach (['table', 'sort', 'dir', 'size', 'page', 'hide'] as $k) {
        if (isset($_POST[$k]) && $_POST[$k] !== '') {
            $back[$k] = (string)$_POST[$k];
        }
    }
    $url = 'table.php' . ($back ? '?' . http_build_query($back) : '');

    $msg = 'Nothing deleted: invalid request.';
    try {
        $ptable = find_table($conn, $_POST['table'] ?? '');
        $pn = (int)($_POST['n'] ?? 0);
        if ($pn > 500000) {
            $pn = 500000;
        }
        if ($ptable && $pn >= 1) {
            list(, $pcols) = db_fetch(
                $conn,
                'SELECT column_name FROM user_tab_columns WHERE table_name = :t ORDER BY column_id',
                [':t' => $ptable]
            );
            $pnames = array_column($pcols, 'COLUMN_NAME');
            $psort = in_array($_POST['sort'] ?? '', $pnames, true) ? $_POST['sort'] : ($pnames[0] ?? null);
            $pdir = (($_POST['dir'] ?? 'DESC') === 'ASC') ? 'ASC' : 'DESC';
            $pwhich = (($_POST['which'] ?? 'first') === 'last') ? 'last' : 'first';

            if ($psort) {
                $del_dir = ($pwhich === 'first') ? $pdir : ($pdir === 'ASC' ? 'DESC' : 'ASC');
                list(, $rng) = db_fetch(
                    $conn,
                    "SELECT MIN({$psort}) AS lo, MAX({$psort}) AS hi
                       FROM (SELECT {$psort} FROM {$ptable}
                              ORDER BY {$psort} {$del_dir}
                              FETCH FIRST {$pn} ROWS ONLY)"
                );
                $deleted = db_exec(
                    $conn,
                    "DELETE FROM {$ptable}
                      WHERE ROWID IN (SELECT ROWID FROM {$ptable}
                                       ORDER BY {$psort} {$del_dir}
                                       FETCH FIRST {$pn} ROWS ONLY)"
                );
                $range = $deleted > 0 ? ' (' . $psort . ' ' . $rng[0]['LO'] . ' to ' . $rng[0]['HI'] . ')' : '';
                $msg = 'Deleted ' . $deleted . ' row' . ($deleted === 1 ? '' : 's') . ' from ' . $ptable . $range . '.';
            }
        }
    } catch (RuntimeException $ex) {
        $msg = 'Delete failed: ' . $ex->getMessage();
    }

    header('Location: ' . $url . (strpos($url, '?') === false ? '?' : '&') . 'msg=' . rawurlencode($msg));
    exit;
}

try {
    list(, $tables) = db_fetch(
        $conn,
        "SELECT t.table_name,
                t.num_rows,
                t.last_analyzed,
                TO_CHAR(o.created, 'YYYY-MM-DD HH24:MI:SS') AS created_at
           FROM user_tables t
           JOIN user_objects o ON o.object_name = t.table_name AND o.object_type = 'TABLE'
          WHERE t.table_name NOT LIKE 'BIN$%'"
    );

    foreach ($tables as &$t) {
        $tname = $t['TABLE_NAME'];
        $last_upd = null;

        // 1. Try SCN_TO_TIMESTAMP (exact physical commit time of newest row)
        try {
            list(, $r) = db_fetch($conn, "SELECT TO_CHAR(SCN_TO_TIMESTAMP(MAX(ora_rowscn)), 'YYYY-MM-DD HH24:MI:SS') AS lu FROM {$tname}");
            if (!empty($r[0]['LU'])) {
                $last_upd = $r[0]['LU'];
            }
        } catch (Exception $e) {}

        // 2. Check table timestamp column if present
        list(, $cols) = db_fetch($conn, "SELECT column_name FROM user_tab_columns WHERE table_name = :t", [':t' => $tname]);
        $cnames = array_column($cols, 'COLUMN_NAME');
        $time_col = null;
        foreach (['CAPTURE_TIME', 'LOG_TIME', 'CREATED_AT', 'TIME', 'TS', 'TIMESTAMP'] as $tc) {
            if (in_array($tc, $cnames, true)) {
                $time_col = $tc;
                break;
            }
        }
        if ($time_col) {
            $sort_col = in_array('LOG_ID', $cnames, true) ? 'LOG_ID' : 'ROWID';
            try {
                list(, $rr) = db_fetch($conn, "SELECT {$time_col} AS lu FROM {$tname} ORDER BY {$sort_col} DESC FETCH FIRST 1 ROWS ONLY");
                if (!empty($rr[0]['LU'])) {
                    $row_val = substr((string)$rr[0]['LU'], 0, 19);
                    if (preg_match('/^\d{4}-\d{2}-\d{2}/', $row_val)) {
                        if ($last_upd === null || strcmp($row_val, $last_upd) > 0) {
                            $last_upd = $row_val;
                        }
                    }
                }
            } catch (Exception $e) {}
        }

        // Fallback to table creation date if empty or unwritten
        if (!$last_upd) {
            $last_upd = $t['CREATED_AT'];
        }
        $t['LAST_UPDATED'] = $last_upd;
    }
    unset($t);

    // Sort tables by LAST_UPDATED DESC
    usort($tables, function ($a, $b) {
        return strcmp((string)$b['LAST_UPDATED'], (string)$a['LAST_UPDATED']);
    });

    list(, $gtabs) = db_fetch(
        $conn,
        "SELECT table_name
           FROM user_tab_columns
          WHERE column_name IN ('CAPTURE_TIME','BATT_PCT','DRAM_USED_PCT','PSRAM_USED_PCT')
          GROUP BY table_name
         HAVING COUNT(DISTINCT column_name) = 4"
    );
    $graphable = array_flip(array_column($gtabs, 'TABLE_NAME'));
} catch (RuntimeException $ex) {
    die('<h2>Could not list tables</h2><pre>' . esc($ex->getMessage()) . '</pre>');
}

$selected = find_table($conn, $_GET['table'] ?? '');
$page_size = isset($_GET['size']) ? (int)$_GET['size'] : 25;
if (!in_array($page_size, [10, 25, 50, 100, 250], true)) {
    $page_size = 25;
}
$page = isset($_GET['page']) ? max(1, (int)$_GET['page']) : 1;
$hide = [];
if (isset($_GET['hide']) && $selected) {
    $hide = array_flip(array_filter(explode(',', (string)$_GET['hide'])));
}

$columns = [];
$rows = [];
$total = 0;
$sort_col = null;
$sort_dir = 'DESC';
$err = null;

if ($selected) {
    try {
        list(, $cols) = db_fetch(
            $conn,
            'SELECT column_name, data_type, nullable, column_id
               FROM user_tab_columns WHERE table_name = :t
              ORDER BY column_id',
            [':t' => $selected]
        );
        $columns = $cols;

        $sort_col = isset($_GET['sort']) && in_array($_GET['sort'], array_column($columns, 'COLUMN_NAME'), true)
            ? $_GET['sort']
            : $columns[0]['COLUMN_NAME'];
        $sort_dir = (($_GET['dir'] ?? 'DESC') === 'ASC') ? 'ASC' : 'DESC';

        list(, $cnt) = db_fetch($conn, "SELECT COUNT(*) AS c FROM {$selected}");
        $total = (int)$cnt[0]['C'];

        // Clamp before querying, otherwise an out-of-range page yields an empty
        // result set while the pager still reports a valid page.
        $total_pages = $total > 0 ? (int)ceil($total / $page_size) : 1;
        $page = min($page, $total_pages);

        $visible = array_values(array_filter(
            array_column($columns, 'COLUMN_NAME'),
            function ($c) use ($hide) {
                return !isset($hide[$c]);
            }
        ));
        if (!$visible) {
            $visible = array_column($columns, 'COLUMN_NAME');
        }

        $select_list = implode(', ', $visible);
        $offset = ($page - 1) * $page_size;
        list(, $rows) = db_fetch(
            $conn,
            "SELECT {$select_list} FROM {$selected}
              ORDER BY {$sort_col} {$sort_dir}
              OFFSET {$offset} ROWS FETCH NEXT {$page_size} ROWS ONLY"
        );
    } catch (RuntimeException $ex) {
        $err = $ex->getMessage();
        $columns = [];
        $rows = [];
    }
}

$total_pages = $total > 0 ? (int)ceil($total / $page_size) : 1;
$page = min($page, max(1, $total_pages));

function qs(array $over)
{
    global $selected, $page_size, $hide;
    $base = ['table' => $selected, 'size' => $page_size];
    if ($hide) {
        $base['hide'] = implode(',', array_keys($hide));
    }
    $base = array_merge($base, $over);
    return '?' . http_build_query($base);
}

$me = htmlspecialchars($_SERVER['PHP_SELF'] ?? 'table.php', ENT_QUOTES);
?>
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <title>Oracle Tables</title>
    <style>
        body { font-family: -apple-system, "Segoe UI", Roboto, sans-serif; margin: 1.5rem; color: #1a1a1a; }
        h1 { font-size: 1.4rem; margin-bottom: 0.25rem; }
        .sub { color: #666; font-size: 0.85rem; margin-bottom: 1.25rem; }
        a { color: #0b57d0; text-decoration: none; }
        a:hover { text-decoration: underline; }
        table { border-collapse: collapse; font-size: 0.82rem; }
        th, td { border: 1px solid #d0d0d0; padding: 4px 8px; text-align: left; vertical-align: top;
                 max-width: 340px; overflow-wrap: anywhere; }
        th { background: #f4f5f7; position: sticky; top: 0; }
        th a { color: #1a1a1a; }
        td.null { color: #999; font-style: italic; }
        .cols { margin: 0.75rem 0; font-size: 0.82rem; display: flex; flex-wrap: wrap; gap: 0.75rem; }
        .cols a { display: inline-block; padding: 1px 6px; border: 1px solid #d0d0d0; border-radius: 3px; background: #fafafa; }
        .bar { display: flex; align-items: center; gap: 1rem; margin: 0.75rem 0; flex-wrap: wrap; font-size: 0.85rem; }
        select, button { font-size: 0.85rem; padding: 2px 6px; }
        .err { background: #fdecea; border: 1px solid #f5c2c0; padding: 0.75rem; border-radius: 4px; }
        .ok { background: #e6f4ea; border: 1px solid #ceead6; padding: 0.75rem; border-radius: 4px; }
        code { background: #f4f5f7; padding: 1px 4px; border-radius: 3px; }
    </style>
</head>
<body>

<h1>Oracle Tables</h1>
<?php if (!empty($_GET['msg'])): ?>
    <p class="ok"><?php echo esc($_GET['msg']); ?></p>
<?php endif; ?>
<p class="sub">
    <?php echo count($tables); ?> table<?php echo count($tables) === 1 ? '' : 's'; ?> in
    <code><?php echo esc(strtolower(db_fetch($conn, 'SELECT user FROM dual')[1][0]['USER'])); ?></code>
    &middot; logged into by <a href="myapi.php">myapi.php</a>
</p>

<?php if (!$selected): ?>

    <table>
        <thead>
            <tr>
                <th>Table</th>
                <th>Created</th>
                <th>Last updated</th>
                <th>Approx. rows</th>
                <th>Last analyzed</th>
            </tr>
        </thead>
        <tbody>
        <?php foreach ($tables as $t): ?>
            <tr>
                <td><a href="<?php echo qs(['table' => $t['TABLE_NAME']]); ?>"><?php echo esc($t['TABLE_NAME']); ?></a><?php if (isset($graphable[$t['TABLE_NAME']])): ?> &middot; <a href="graph.php?table=<?php echo urlencode($t['TABLE_NAME']); ?>">graph</a><?php endif; ?></td>
                <td><?php echo $t['CREATED_AT'] ? esc($t['CREATED_AT']) : '-'; ?></td>
                <td><strong><?php echo $t['LAST_UPDATED'] ? esc($t['LAST_UPDATED']) : '-'; ?></strong></td>
                <td><?php echo $t['NUM_ROWS'] === null ? '-' : esc($t['NUM_ROWS']); ?></td>
                <td><?php echo $t['LAST_ANALYZED'] ? esc(substr((string)$t['LAST_ANALYZED'], 0, 19)) : '-'; ?></td>
            </tr>
        <?php endforeach; ?>
        <?php if (!$tables): ?>
            <tr><td colspan="5">No tables yet. POST or GET to
                <code>myapi.php?table=YOUR_TABLE&amp;col=value</code> to create one.</td></tr>
        <?php endif; ?>
        </tbody>
    </table>

<?php else: ?>

    <p class="sub"><a href="<?php echo $me; ?>">&larr; all tables</a>
    <?php if (isset($graphable[$selected])): ?>
        &middot; <a href="graph.php?table=<?php echo urlencode($selected); ?>">device graph &rarr;</a>
    <?php endif; ?>
    </p>

    <?php if ($err): ?>
        <p class="err">Query failed: <?php echo esc($err); ?></p>
    <?php endif; ?>

    <?php if ($columns): ?>
    <div class="cols">
        <?php
        $all_visible = array_keys($hide);
        if ($all_visible) {
            echo '<a href="' . esc(qs(['hide' => ''])) . '">show all columns</a>';
        }
        foreach ($columns as $c):
            $name = $c['COLUMN_NAME'];
            $is_hidden = isset($hide[$name]);
            $next = $is_hidden ? array_diff_key($hide, [$name => 1]) : $hide + [$name => 1];
            ?>
            <a href="<?php echo esc(qs(['hide' => implode(',', array_keys($next))])); ?>">
                <?php echo $is_hidden ? '+' : '&minus;'; ?> <?php echo esc($name); ?>
                <span style="color:#888">(<?php echo esc(strtolower($c['DATA_TYPE'])); ?>)</span>
            </a>
        <?php endforeach; ?>
    </div>

    <div class="bar">
        <span><strong><?php echo number_format($total); ?></strong> row<?php echo $total === 1 ? '' : 's'; ?></span>
        <form method="get" style="display:inline">
            <input type="hidden" name="table" value="<?php echo esc($selected); ?>">
            <?php if ($hide): ?><input type="hidden" name="hide" value="<?php echo esc(implode(',', array_keys($hide))); ?>"><?php endif; ?>
            <label>Rows
                <select name="size" onchange="this.form.submit()">
                    <?php foreach ([10, 25, 50, 100, 250] as $s): ?>
                        <option value="<?php echo $s; ?>" <?php echo $s === $page_size ? 'selected' : ''; ?>><?php echo $s; ?></option>
                    <?php endforeach; ?>
                </select>
            </label>
        </form>
        <span>
            <?php if ($page > 1): ?>
                <a href="<?php echo esc(qs(['page' => 1])); ?>">&laquo; first</a>
                <a href="<?php echo esc(qs(['page' => $page - 1])); ?>">&lsaquo; prev</a>
            <?php endif; ?>
            page <?php echo $page; ?> of <?php echo number_format($total_pages); ?>
            <?php if ($page < $total_pages): ?>
                <a href="<?php echo esc(qs(['page' => $page + 1])); ?>">next &rsaquo;</a>
                <a href="<?php echo esc(qs(['page' => $total_pages])); ?>">last &raquo;</a>
            <?php endif; ?>
        </span>
        <span>
            <form method="post" id="pruneform" onsubmit="return pruneConfirm()" style="display:inline">
                <input type="hidden" name="do_prune" value="1">
                <input type="hidden" name="table" value="<?php echo esc($selected); ?>">
                <input type="hidden" name="sort" value="<?php echo esc((string)$sort_col); ?>">
                <input type="hidden" name="dir" value="<?php echo esc($sort_dir); ?>">
                <input type="hidden" name="hide" value="<?php echo esc(implode(',', array_keys($hide))); ?>">
                <input type="hidden" name="size" value="<?php echo (int)$page_size; ?>">
                <input type="hidden" name="page" value="<?php echo (int)$page; ?>">
                <input type="hidden" name="which" id="which" value="first">
                <input type="number" name="n" value="100" min="1" max="500000" style="width:6em" aria-label="rows to delete">
                &nbsp;rows &middot;
                <button type="submit" onclick="armPrune('first')">delete first</button>
                <button type="submit" onclick="armPrune('last')">delete last</button>
            </form>
        </span>
    </div>

    <table>
        <thead>
            <tr>
            <?php foreach ($columns as $c):
                $name = $c['COLUMN_NAME'];
                if (isset($hide[$name])) { continue; }
                $next_dir = ($sort_col === $name && $sort_dir === 'DESC') ? 'ASC' : 'DESC';
                ?>
                <th><a href="<?php echo esc(qs(['sort' => $name, 'dir' => $next_dir, 'page' => 1])); ?>">
                    <?php echo esc($name); ?>
                    <?php if ($sort_col === $name): ?><?= $sort_dir === 'ASC' ? '&uarr;' : '&darr;' ?><?php endif; ?>
                </a></th>
            <?php endforeach; ?>
            </tr>
        </thead>
        <tbody>
        <?php foreach ($rows as $row): ?>
            <tr>
            <?php foreach (array_keys($row) as $name): ?>
                <td<?php echo $row[$name] === null ? ' class="null"' : ''; ?>>
                    <?php echo $row[$name] === null ? 'null' : esc($row[$name]); ?>
                </td>
            <?php endforeach; ?>
            </tr>
        <?php endforeach; ?>
        <?php if (!$rows): ?>
            <tr><td colspan="<?php echo count($columns); ?>">No rows.</td></tr>
        <?php endif; ?>
        </tbody>
    </table>
    <?php endif; ?>

<?php endif; ?>

<script>
function armPrune(which) {
    document.getElementById('which').value = which;
}
function pruneConfirm() {
    const f = document.getElementById('pruneform');
    const n = parseInt(f.n.value, 10);
    if (!(n >= 1)) {
        alert('Enter a row count of 1 or more.');
        return false;
    }
    const pos = f.which.value === 'last' ? 'LAST' : 'FIRST';
    const where = f.which.value === 'last' ? 'bottom' : 'top';
    return confirm('Delete the ' + pos + ' ' + n + ' rows of ' + f.table.value +
        ' (sorted by ' + f.sort.value + ' ' + f.dir.value + ' - the rows at the ' + where +
        ' of the table)?');
}
</script>

</body>
</html>