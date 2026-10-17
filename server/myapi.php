<?php
/**
 * myapi.php - request logger with optional database table writes.
 * Supports single row (GET/POST) and batch array ingestion (JSON POST).
 *
 * Always logs every request to a flat file and to the API_LOG table.
 * Additionally, when the caller passes `table`, it inserts or updates rows in
 * that table, creating it first if it does not exist yet.
 */

require_once '/var/www/conf/db_connect.php';

header('Content-Type: application/json');

$log_file = '/var/www/html/myapi-log.txt';
$default_table = 'API_LOG';

$raw_input = file_get_contents('php://input');
$json_input = !empty($raw_input) ? json_decode($raw_input, true) : null;

$urlDetails = [
    'scheme' => isset($_SERVER['REQUEST_SCHEME']) ? $_SERVER['REQUEST_SCHEME'] : 'http',
    'host'   => $_SERVER['HTTP_HOST'] ?? '-',
    'path'   => $_SERVER['REQUEST_URI'] ?? '/',
    'query'  => $_SERVER['QUERY_STRING'] ?? '',
];

$params = [
    'GET'  => $_GET,
    'POST' => $_POST,
    'JSON' => $json_input !== null ? '[json present]' : null,
];

// ---------------------------------------------------------------- file log
$logEntry = sprintf(
    "[%s] %s %s %s\n",
    date("Y-m-d H:i:s"),
    $urlDetails['scheme'],
    $urlDetails['host'] . $urlDetails['path'],
    http_build_query($params)
);

$fh = @fopen($log_file, 'a');
if ($fh) {
    @flock($fh, LOCK_EX);
    @fwrite($fh, $logEntry);
    @flock($fh, LOCK_UN);
    @fclose($fh);
}

// ------------------------------------------------------------- db plumbing
$db_status = 'not attempted';
$db_detail = null;
$db_action = null;

if (!function_exists('oci_connect')) {
    $db_status = 'skipped: oci8 extension not loaded';
    $response = [
        'status' => 'ok',
        'logged_to' => $log_file,
        'database' => $db_status,
    ];
    echo json_encode($response);
    exit;
}

/** Identifiers cannot be bound, so they are whitelisted instead. */
function safe_identifier($name)
{
    $name = strtoupper(trim((string)$name));
    if (!preg_match('/^[A-Z][A-Z0-9_]{0,29}$/', $name)) {
        throw new InvalidArgumentException("Invalid table or column name: {$name}");
    }
    return $name;
}

/** True if the table is present in the current schema. */
function table_exists($conn, $table)
{
    $stid = oci_parse($conn, "SELECT table_name FROM user_tables WHERE table_name = :t");
    oci_bind_by_name($stid, ':t', $table);
    oci_execute($stid);
    $found = oci_fetch_assoc($stid) ? true : false;
    oci_free_statement($stid);
    return $found;
}

/** True if the column is present on the table. */
function column_exists($conn, $table, $column)
{
    $stid = oci_parse($conn, "SELECT column_name FROM user_tab_columns
                              WHERE table_name = :t AND column_name = :c");
    oci_bind_by_name($stid, ':t', $table);
    oci_bind_by_name($stid, ':c', $column);
    oci_execute($stid);
    $found = oci_fetch_assoc($stid) ? true : false;
    oci_free_statement($stid);
    return $found;
}

/**
 * Make the table match the incoming params: create it if absent, otherwise
 * ALTER it to add any columns that are new.
 */
function ensure_table($conn, $table, array $data_cols, $key_col = null)
{
    if ($key_col && !in_array($key_col, $data_cols, true)) {
        array_unshift($data_cols, $key_col);
    }
    $data_cols = array_unique($data_cols);

    if (!table_exists($conn, $table)) {
        $defs = ["LOG_ID NUMBER GENERATED ALWAYS AS IDENTITY PRIMARY KEY"];
        foreach ($data_cols as $col) {
            $defs[] = $col . ' VARCHAR2(4000)';
        }
        $defs[] = 'CREATED_AT TIMESTAMP DEFAULT SYSTIMESTAMP';
        $defs[] = 'UPDATED_AT TIMESTAMP DEFAULT SYSTIMESTAMP';

        db_exec($conn, 'CREATE TABLE ' . $table . ' (' . implode(', ', $defs) . ')');
        return ['created' => true, 'added' => $data_cols];
    }

    $added = [];
    foreach ($data_cols as $col) {
        if (column_exists($conn, $table, $col)) {
            continue;
        }
        db_exec($conn, "ALTER TABLE {$table} ADD ({$col} VARCHAR2(4000))");
        $added[] = $col;
    }

    return ['created' => false, 'added' => $added];
}

function ensure_device_auth_table($conn) {
    static $ensured = false;
    if ($ensured) return;
    if (!table_exists($conn, 'TB_DEVICE_AUTH')) {
        $sql = "CREATE TABLE TB_DEVICE_AUTH (
            TABLE_NAME VARCHAR2(32) PRIMARY KEY,
            AUTH_SEED VARCHAR2(64),
            LAST_SEEN TIMESTAMP DEFAULT SYSTIMESTAMP,
            CREATED_AT TIMESTAMP DEFAULT SYSTIMESTAMP,
            IP VARCHAR2(64),
            FW VARCHAR2(64),
            HITS NUMBER DEFAULT 1
        )";
        try {
            db_exec($conn, $sql);
        } catch (Exception $e) {
            // ignore if created concurrently
        }
    }
    $ensured = true;
}

function register_device_auth($conn, $table, $auth_seed = null, $fw = null) {
    if (empty($table) || !str_starts_with(strtoupper($table), 'TB_')) {
        return;
    }
    $table = strtoupper(trim((string)$table));
    ensure_device_auth_table($conn);

    $ip = $_SERVER['REMOTE_ADDR'] ?? '-';
    $fw = $fw ?? 'telemetry';
    $seed = (!empty($auth_seed) && is_string($auth_seed)) ? trim($auth_seed) : null;

    $mergeSql = "MERGE INTO TB_DEVICE_AUTH d
        USING (SELECT :tbl AS table_name FROM dual) s
        ON (d.table_name = s.table_name)
        WHEN MATCHED THEN
            UPDATE SET d.last_seen = SYSTIMESTAMP,
                       d.ip = :ip,
                       d.hits = NVL(d.hits, 0) + 1,
                       d.auth_seed = CASE WHEN :seed IS NOT NULL AND LENGTH(:seed) > 0 THEN :seed ELSE d.auth_seed END,
                       d.fw = :fw
        WHEN NOT MATCHED THEN
            INSERT (table_name, auth_seed, last_seen, created_at, ip, fw, hits)
            VALUES (:tbl, :seed, SYSTIMESTAMP, SYSTIMESTAMP, :ip, :fw, 1)";

    try {
        $stid = oci_parse($conn, $mergeSql);
        oci_bind_by_name($stid, ':tbl', $table);
        oci_bind_by_name($stid, ':ip', $ip);
        oci_bind_by_name($stid, ':seed', $seed);
        oci_bind_by_name($stid, ':fw', $fw);
        oci_execute($stid, OCI_COMMIT_ON_SUCCESS);
        oci_free_statement($stid);
    } catch (Exception $e) {
        error_log("register_device_auth failed: " . $e->getMessage());
    }
}

try {
    $conn = db_connect();

    // Check if request is a batch array
    $batch_rows = null;
    if (is_array($json_input)) {
        if (isset($json_input['rows']) && is_array($json_input['rows'])) {
            $batch_rows = $json_input['rows'];
        } elseif (isset($json_input['batch']) && is_array($json_input['batch'])) {
            $batch_rows = $json_input['batch'];
        } elseif (isset($json_input[0]) && is_array($json_input[0])) {
            $batch_rows = $json_input;
        }
    }

    if ($batch_rows !== null) {
        // --- BATCH INGESTION ---
        $table = safe_identifier($json_input['table'] ?? $_GET['table'] ?? $_POST['table'] ?? $default_table);
        $auth_seed = $json_input['auth_seed'] ?? $_GET['auth_seed'] ?? $_POST['auth_seed'] ?? null;
        register_device_auth($conn, $table, $auth_seed);
        $all_cols = [];
        foreach ($batch_rows as $row) {
            if (is_array($row)) {
                foreach (array_keys($row) as $k) {
                    $all_cols[safe_identifier($k)] = true;
                }
            }
        }
        $data_cols = array_keys($all_cols);
        if (empty($data_cols)) {
            throw new InvalidArgumentException('Batch contains no valid data columns');
        }

        $schema = ensure_table($conn, $table, $data_cols, null);

        $placeholders = [];
        foreach ($data_cols as $c) {
            $placeholders[] = ':' . strtolower($c);
        }
        $sql = 'INSERT INTO ' . $table . ' (' . implode(', ', $data_cols) . ') VALUES (' . implode(', ', $placeholders) . ')';
        $stid = oci_parse($conn, $sql);
        if (!$stid) {
            $e = oci_error($conn);
            throw new RuntimeException('Batch SQL parse failed: ' . $e['message']);
        }

        $bind_storage = [];
        foreach ($data_cols as $c) {
            $bind_storage[$c] = '';
            oci_bind_by_name($stid, ':' . strtolower($c), $bind_storage[$c], 4000);
        }

        $inserted_count = 0;
        foreach ($batch_rows as $row) {
            if (!is_array($row)) continue;
            foreach ($data_cols as $c) {
                $val = null;
                foreach ($row as $rk => $rv) {
                    if (strcasecmp($rk, $c) === 0) {
                        $val = is_array($rv) ? json_encode($rv) : (string)$rv;
                        break;
                    }
                }
                $bind_storage[$c] = $val;
            }
            if (!oci_execute($stid, OCI_NO_AUTO_COMMIT)) {
                $e = oci_error($stid);
                oci_rollback($conn);
                oci_free_statement($stid);
                throw new RuntimeException('Batch insert failed on item ' . $inserted_count . ': ' . $e['message']);
            }
            $inserted_count++;
        }
        oci_commit($conn);
        oci_free_statement($stid);

        $db_status = 'ok';
        $db_detail = [
            'table'   => $table,
            'mode'    => 'batch_insert',
            'action'  => "inserted {$inserted_count} rows",
            'count'   => $inserted_count,
            'created' => $schema['created'] ? 'table did not exist, created now' : 'table already existed',
        ];
        if ($schema['added']) {
            $db_detail['columns_added'] = $schema['added'];
        }
    } else {
        // --- SINGLE ROW INSERT / UPDATE / UPSERT ---
        $source = $_POST ? $_POST : $_GET;
        if (is_array($json_input)) {
            $source = array_merge($source, $json_input);
        }

        $mode  = strtolower((string)($source['mode'] ?? 'insert'));
        $table = safe_identifier($source['table'] ?? $default_table);
        $auth_seed = $source['auth_seed'] ?? null;
        register_device_auth($conn, $table, $auth_seed);
        $key   = $source['key'] ?? null;
        $value = $source['value'] ?? null;

        if (!in_array($mode, ['insert', 'update', 'upsert'], true)) {
            throw new InvalidArgumentException("mode must be insert, update or upsert");
        }
        $is_keyed = ($mode === 'update' || $mode === 'upsert');
        if ($is_keyed && !$key) {
            throw new InvalidArgumentException("mode={$mode} requires a 'key' column");
        }
        $key = $is_keyed ? safe_identifier($key) : null;

        $control = ['mode', 'table'];
        if ($is_keyed) {
            $control[] = 'key';
            $control[] = 'value';
        }

        $data = [];
        foreach ($source as $k => $v) {
            if (in_array(strtolower($k), $control, true)) {
                continue;
            }
            if (is_array($v)) {
                $v = json_encode($v);
            }
            $data[safe_identifier($k)] = (string)$v;
        }
        if ($key && $value !== null && !isset($data[$key])) {
            $data[$key] = (string)$value;
        }

        $schema = ensure_table($conn, $table, array_keys($data), $key);

        if ($mode === 'insert') {
            $cols = array_keys($data);
            if (!$cols) {
                throw new InvalidArgumentException('Nothing to insert: no data params supplied');
            }
            $bind = [];
            foreach ($data as $c => $v) {
                $bind[':' . strtolower($c)] = $v;
            }
            $sql = 'INSERT INTO ' . $table . ' (' . implode(', ', $cols) . ') VALUES ('
                 . implode(', ', array_map(function ($c) { return ':' . strtolower($c); }, $cols)) . ')';
            db_exec($conn, $sql, $bind);
            $db_action = 'inserted';
        } elseif ($mode === 'update') {
            $set = $bind = [];
            foreach ($data as $c => $v) {
                if (!column_exists($conn, $table, $c)) {
                    continue;
                }
                $set[] = "$c = :" . strtolower($c);
                $bind[':' . strtolower($c)] = $v;
            }
            if (!$set) {
                throw new RuntimeException('No writable columns on ' . $table);
            }
            $bind[':wkey'] = (string)$value;
            $affected = db_exec(
                $conn,
                'UPDATE ' . $table . ' SET ' . implode(', ', $set)
                . ", UPDATED_AT = SYSTIMESTAMP WHERE {$key} = :wkey",
                $bind
            );
            $db_action = $affected ? 'updated' : 'no matching row';
        } else { // upsert via MERGE
            $cols = array_keys($data);
            if (!$cols) {
                throw new InvalidArgumentException('Nothing to upsert: no data params supplied');
            }
            $bind = [];
            $set = [];
            foreach ($data as $c => $v) {
                $bind[':' . strtolower($c)] = $v;
                if ($c !== $key) {
                    $set[] = 't.' . $c . ' = :' . strtolower($c);
                }
            }
            $src_cols = array_keys($data);
            $src_sel = implode(', ', array_map(
                function ($c) { return ':' . strtolower($c) . ' AS ' . $c; },
                $src_cols
            ));
            $sql = 'MERGE INTO ' . $table . ' t '
                 . 'USING (SELECT ' . $src_sel . ' FROM dual) s '
                 . 'ON (t.' . $key . ' = s.' . $key . ') '
                 . 'WHEN MATCHED THEN UPDATE SET ' . implode(', ', $set) . ', t.UPDATED_AT = SYSTIMESTAMP '
                 . 'WHEN NOT MATCHED THEN INSERT (' . implode(', ', $src_cols) . ') VALUES ('
                 . implode(', ', array_map(function ($c) { return 's.' . $c; }, $src_cols)) . ')';
            db_exec($conn, $sql, $bind);
            $db_action = 'upserted';
        }

        $db_status = 'ok';
        $db_detail = [
            'table'   => $table,
            'mode'    => $mode,
            'action'  => $db_action,
            'created' => $schema['created'] ? 'table did not exist, created now' : 'table already existed',
        ];
        if ($schema['added']) {
            $db_detail['columns_added'] = $schema['added'];
        }
    }

    oci_close($conn);
} catch (InvalidArgumentException $ex) {
    http_response_code(400);
    $db_status = 'rejected: ' . $ex->getMessage();
} catch (RuntimeException $ex) {
    $db_status = 'error: ' . $ex->getMessage();
} catch (Throwable $ex) {
    $db_status = 'error: ' . $ex->getMessage();
}

$response = [
    'status'     => 'ok',
    'logged_to'  => $log_file,
    'database'   => $db_status,
];

if ($db_detail !== null) {
    $response['table_write'] = $db_detail;
}

echo json_encode($response);
