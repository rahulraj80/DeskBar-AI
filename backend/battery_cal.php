<?php
/**
 * battery_cal.php - Dynamic Battery Discharge Calibration & Fleet Learning Service
 * 
 * Capabilities:
 * 1. Analyzes specific device telemetry table (TB_<ID>) for continuous discharge runs (>=70% capacity).
 * 2. If insufficient data for device, discovers up to 10 qualifying fleet tables and computes golden fleet average.
 * 3. Supports POST with raw discharge points to return fitted curve.
 */

require_once '/var/www/conf/db_connect.php';

header('Content-Type: application/json');
header('Access-Control-Allow-Origin: *');

$target_table = isset($_GET['table']) ? strtoupper(trim($_GET['table'])) : '';
// Standard 14-point SOC percentage target vector
$target_pcts = [100, 95, 90, 85, 80, 70, 60, 50, 40, 30, 20, 10, 5, 0];

// Fallback factory curve if nothing in DB
$factory_curve = [
    'volts' => [4.065, 4.000, 3.970, 3.945, 3.930, 3.920, 3.910, 3.885, 3.840, 3.780, 3.735, 3.690, 3.500, 3.100],
    'pcts'  => [100, 95, 90, 85, 80, 70, 60, 50, 40, 30, 20, 10, 5, 0]
];

function sanitize_table_name($name) {
    if (preg_match('/^[A-Z0-9_]{3,30}$/', $name)) {
        return $name;
    }
    return '';
}

/**
 * Extracts the longest continuous discharge run from a list of rows.
 * Continuous discharge: starts >= 1315 mV, ends <= 1235 mV (or drops >= 65 mV),
 * without charging spikes (> 25 mV jump upwards).
 */
function extract_best_discharge_run($rows) {
    if (empty($rows)) return null;

    $runs = [];
    $current_run = [];

    for ($i = 0; $i < count($rows); $i++) {
        $r = $rows[$i];
        $adc = intval($r['BATT_MV']);

        if ($adc > 1410 || $adc < 950) {
            continue; // Skip invalid or USB-boosted ADC points
        }

        if (empty($current_run)) {
            if ($adc >= 1315) {
                $current_run[] = $r;
            }
        } else {
            $prev_adc = intval(end($current_run)['BATT_MV']);
            // If sharp rise (> 25 mV) occurs, charger was connected -> end current run
            if ($adc > $prev_adc + 25) {
                if (count($current_run) >= 12) {
                    $runs[] = $current_run;
                }
                $current_run = [];
                if ($adc >= 1315) {
                    $current_run[] = $r;
                }
            } else {
                $current_run[] = $r;
            }
        }
    }
    if (count($current_run) >= 12) {
        $runs[] = $current_run;
    }

    if (empty($runs)) return null;

    // Pick run with maximum voltage drop span (depth of discharge)
    $best_run = null;
    $max_drop = 0;
    foreach ($runs as $run) {
        $start_adc = intval($run[0]['BATT_MV']);
        $end_adc   = intval(end($run)['BATT_MV']);
        $drop = $start_adc - $end_adc;
        if ($drop > $max_drop) {
            $max_drop = $drop;
            $best_run = $run;
        }
    }

    // Require at least 65 mV drop (~60-70% capacity)
    if ($max_drop < 65) return null;

    return $best_run;
}

/**
 * Reference LiPo OCV curve (the factory golden baseline). Used ONLY to anchor
 * an empirical run: a run that fell 4.17V -> 4.11V covers a few percent of the
 * pack, not 100% -> 0%.
 */
function ref_soc_percent($v, $ref_volts, $pcts) {
    $n = count($ref_volts);
    if ($v >= $ref_volts[0]) return (float)$pcts[0];
    if ($v <= $ref_volts[$n - 1]) return (float)$pcts[$n - 1];
    for ($i = 0; $i < $n - 1; $i++) {
        if ($v <= $ref_volts[$i] && $v >= $ref_volts[$i + 1]) {
            $dv = $ref_volts[$i] - $ref_volts[$i + 1];
            $w = ($dv > 0) ? (($ref_volts[$i] - $v) / $dv) : 0.0;
            return $pcts[$i + 1] + $w * ($pcts[$i] - $pcts[$i + 1]);
        }
    }
    return (float)$pcts[$n - 1];
}

/**
 * Physical invariants (mirrored in the firmware): a curve whose 50% point sits
 * near full charge, or that dumps most of the pack in one 5% step, is a fit
 * artefact rather than a usable calibration. A pack that idles at 4.13V for
 * long stretches produces exactly that, so reject it and let the caller fall
 * through to the next source.
 */
function curve_shape_ok($volts, $pcts) {
    $n = count($volts);
    if ($n < 2) return false;
    $mid = intdiv($n, 2);
    foreach ($pcts as $i => $pct) {
        if ($pct == 50) { $mid = $i; break; }
    }
    if ($volts[$mid] > 4.05 || $volts[$mid] < 3.50) return false;
    for ($i = 0; $i < $n - 1; $i++) {
        if (($volts[$i] - $volts[$i + 1]) > 0.75) return false;
    }
    return true;
}

/** Reference voltage at a target percentage (piecewise linear, clamped). */
function ref_voltage_for($pct, $ref_volts, $pcts) {
    $n = count($ref_volts);
    if ($pct >= $pcts[0]) return $ref_volts[0];
    if ($pct <= $pcts[$n - 1]) return $ref_volts[$n - 1];
    for ($i = 0; $i < $n - 1; $i++) {
        if ($pct <= $pcts[$i] && $pct >= $pcts[$i + 1]) {
            $dp = $pcts[$i] - $pcts[$i + 1];
            $w = ($dp > 0) ? (($pcts[$i] - $pct) / $dp) : 0.0;
            return $ref_volts[$i + 1] + $w * ($ref_volts[$i] - $ref_volts[$i + 1]);
        }
    }
    return $ref_volts[$n - 1];
}

/**
 * Fits 14 target points to an empirical discharge run.
 */
function fit_14_point_curve($run, $target_pcts) {
    $t0 = intval($run[0]['UPTIME_S']);
    $t_end = intval(end($run)['UPTIME_S']);
    $total_time = max(1, $t_end - $t0);

    // Moving average on raw ADC to eliminate instantaneous noise
    $adcs = array_map(function($r) { return intval($r['BATT_MV']); }, $run);
    $uptimes = array_map(function($r) use ($t0) { return intval($r['UPTIME_S']) - $t0; }, $run);
    $count = count($adcs);

    $smoothed_v = [];
    for ($i = 0; $i < $count; $i++) {
        $win = array_slice($adcs, max(0, $i - 2), 5);
        $avg_adc = array_sum($win) / count($win);
        $smoothed_v[] = ($avg_adc * 3.0) / 1000.0;
    }

    // Anchor the run on physical state of charge instead of assuming it spans
    // 100% -> 0%. Mapping elapsed time straight onto the full percentage range
    // is what turned a 60mV sliver into a curve whose 50% point sat at 4.135V
    // and whose final step fell 1.11V - i.e. "4.1V but 4%".
    $ref_volts = [4.065, 4.000, 3.970, 3.945, 3.930, 3.920, 3.910, 3.885,
                  3.840, 3.780, 3.735, 3.690, 3.500, 3.100];
    $v_first = $smoothed_v[0];
    $v_last  = $smoothed_v[count($smoothed_v) - 1];
    $soc_start = ref_soc_percent($v_first, $ref_volts, $target_pcts);
    $soc_end   = ref_soc_percent($v_last, $ref_volts, $target_pcts);

    // Under 20 points of state of charge means the run says nothing about the
    // shape of the pack - reject it so the caller falls through to the fleet.
    if (($soc_start - $soc_end) < 20.0) {
        return null;
    }

    // State of charge at every sample, assuming a roughly constant load.
    $soc_series = [];
    for ($i = 0; $i < $count; $i++) {
        $frac = ($total_time > 0) ? ($uptimes[$i] / (float)$total_time) : 0.0;
        if ($frac < 0.0) $frac = 0.0;
        if ($frac > 1.0) $frac = 1.0;
        $soc_series[] = $soc_start + ($soc_end - $soc_start) * $frac;
    }

    $fitted_volts = [];
    foreach ($target_pcts as $p) {
        $v = null;
        if ($p <= $soc_start && $p >= $soc_end) {
            // Inside the measured window: read the smoothed voltage at the
            // sample whose anchored state of charge crosses this target.
            for ($j = 0; $j < $count - 1; $j++) {
                $s0 = $soc_series[$j];
                $s1 = $soc_series[$j + 1];
                if (($p <= $s0 && $p >= $s1) || ($p >= $s0 && $p <= $s1)) {
                    $ds = $s0 - $s1;
                    $w = ($ds != 0.0) ? (($s0 - $p) / $ds) : 0.0;
                    $v = $smoothed_v[$j] + $w * ($smoothed_v[$j + 1] - $smoothed_v[$j]);
                    break;
                }
            }
        }
        if ($v === null) {
            // Outside the measured window: the reference curve is more
            // trustworthy than an extrapolation.
            $v = ref_voltage_for($p, $ref_volts, $target_pcts);
            if ($p >= $soc_start) $v = max($v, $v_first);
            if ($p <= $soc_end)   $v = min($v, $v_last);
        }
        // 0% is clamped at 3.10V: the pack is an 18650 that cuts out around
        // 3.00-3.12V, so 3.0 as an empty anchor made the last 5% look deeper
        // than the hardware goes. Everything below 3.10V is "empty".
        $fitted_volts[] = min(4.200, max(3.100, round($v, 3)));
    }

    // Ensure strict monotonicity descending
    for ($k = 1; $k < count($fitted_volts); $k++) {
        if ($fitted_volts[$k] >= $fitted_volts[$k-1]) {
            $fitted_volts[$k] = round($fitted_volts[$k-1] - 0.005, 3);
        }
    }

    // Monotonic is not the same as meaningful.
    return curve_shape_ok($fitted_volts, $target_pcts) ? $fitted_volts : null;
}

// Check for direct POST with raw discharge points
if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $raw_input = file_get_contents('php://input');
    $data = json_decode($raw_input, true);
    if (!empty($data)) {
        $rows = [];
        if (isset($data['samples']) && is_array($data['samples'])) {
            $rows = $data['samples'];
        } elseif (is_array($data) && isset($data[0])) {
            $rows = $data;
        }

        // Format rows to expected keys
        $formatted_rows = [];
        $uptime_synth = 0;
        foreach ($rows as $item) {
            $mv = 0;
            if (is_numeric($item)) {
                $mv = intval($item);
            } elseif (is_array($item)) {
                $mv = isset($item['BATT_MV']) ? intval($item['BATT_MV']) : (isset($item['batt_mv']) ? intval($item['batt_mv']) : (isset($item['mv']) ? intval($item['mv']) : 0));
            }
            $uptime = isset($item['UPTIME_S']) ? intval($item['UPTIME_S']) : (isset($item['uptime_s']) ? intval($item['uptime_s']) : $uptime_synth);
            $uptime_synth += 300; // 5 min default if synthesized

            if ($mv > 0) {
                $formatted_rows[] = ['BATT_MV' => $mv, 'UPTIME_S' => $uptime];
            }
        }

        $run = extract_best_discharge_run($formatted_rows);
        if ($run === null && count($formatted_rows) >= 10) {
            // If user explicitly sent a discharge segment, use as-is
            $run = $formatted_rows;
        }

        $fitted = ($run !== null && count($run) >= 10)
            ? fit_14_point_curve($run, $target_pcts) : null;
        if ($fitted !== null) {
            echo json_encode([
                'status' => 'ok',
                'source' => 'raw_post_fit',
                'samples_analyzed' => count($run),
                'volts'  => $fitted,
                'pcts'   => $target_pcts,
                'message' => 'Fitted curve calculated from POSTed raw discharge data'
            ], JSON_PRETTY_PRINT);
            exit;
        } else {
            http_response_code(400);
            echo json_encode([
                'status' => 'error',
                'error' => 'Insufficient discharge data provided (need at least 10 points with measurable discharge drop)'
            ]);
            exit;
        }
    }
}

try {
    $conn = db_connect();

    // 1. Try Requested Device Table
    $clean_table = sanitize_table_name($target_table);
    if (!empty($clean_table)) {
        $stmt = @oci_parse($conn, "SELECT LOG_ID, CAPTURE_TIME, UPTIME_S, BATT_MV FROM {$clean_table} ORDER BY LOG_ID ASC");
        if ($stmt && @oci_execute($stmt)) {
            $device_rows = [];
            while ($r = oci_fetch_assoc($stmt)) {
                $device_rows[] = $r;
            }
            $best_run = extract_best_discharge_run($device_rows);
            $fitted = ($best_run !== null && count($best_run) >= 15)
                ? fit_14_point_curve($best_run, $target_pcts) : null;
            if ($fitted !== null) {
                $run_duration = intval(end($best_run)['UPTIME_S']) - intval($best_run[0]['UPTIME_S']);

                echo json_encode([
                    'status' => 'ok',
                    'source' => 'device_history',
                    'table'  => $clean_table,
                    'samples_analyzed' => count($best_run),
                    'run_duration_min' => round($run_duration / 60),
                    'volts'  => $fitted,
                    'pcts'   => $target_pcts,
                    'message' => "Calibration calculated from local device run ({$clean_table})"
                ], JSON_PRETTY_PRINT);
                exit;
            }
        }
    }

    // 2. Fallback: Fleet Discovery & Averaging (Max 10 Qualifying Tables)
    $stmt_tables = oci_parse($conn, "SELECT TABLE_NAME FROM USER_TABLES WHERE TABLE_NAME LIKE 'TB_%' ORDER BY TABLE_NAME ASC");
    oci_execute($stmt_tables);

    $qualifying_curves = [];
    $tables_checked = 0;

    while ($t_row = oci_fetch_assoc($stmt_tables)) {
        $tb = $t_row['TABLE_NAME'];
        $tables_checked++;

        $s_data = @oci_parse($conn, "SELECT LOG_ID, CAPTURE_TIME, UPTIME_S, BATT_MV FROM {$tb} ORDER BY LOG_ID ASC");
        if ($s_data && @oci_execute($s_data)) {
            $t_rows = [];
            while ($tr = oci_fetch_assoc($s_data)) {
                $t_rows[] = $tr;
            }
            $run = extract_best_discharge_run($t_rows);
            if ($run !== null) {
                $curve = fit_14_point_curve($run, $target_pcts);
                // null = run too shallow to say anything about the pack shape
                if ($curve !== null) {
                    $qualifying_curves[$tb] = $curve;
                    // Limit to maximum 10 qualifying tables
                    if (count($qualifying_curves) >= 10) {
                        break;
                    }
                }
            }
        }
    }

    if (!empty($qualifying_curves)) {
        $num_tables = count($qualifying_curves);
        $avg_volts = [];
        for ($p_idx = 0; $p_idx < count($target_pcts); $p_idx++) {
            $sum = 0.0;
            foreach ($qualifying_curves as $tb_name => $pts) {
                $sum += $pts[$p_idx];
            }
            $avg_volts[] = round($sum / $num_tables, 3);
        }

        // Monotonic check
        for ($k = 1; $k < count($avg_volts); $k++) {
            if ($avg_volts[$k] >= $avg_volts[$k-1]) {
                $avg_volts[$k] = round($avg_volts[$k-1] - 0.005, 3);
            }
        }

        echo json_encode([
            'status' => 'ok',
            'source' => 'fleet_average',
            'contributing_devices' => $num_tables,
            'qualifying_tables' => array_keys($qualifying_curves),
            'volts'  => $avg_volts,
            'pcts'   => $target_pcts,
            'message' => "Calibration computed from fleet average across {$num_tables} devices"
        ], JSON_PRETTY_PRINT);
        exit;
    }

    // 3. Last Resort: Factory Golden Curve
    echo json_encode([
        'status' => 'ok',
        'source' => 'factory_baseline',
        'volts'  => $factory_curve['volts'],
        'pcts'   => $factory_curve['pcts'],
        'message' => "Baseline factory profile"
    ], JSON_PRETTY_PRINT);

} catch (Exception $e) {
    http_response_code(500);
    echo json_encode([
        'status' => 'error',
        'error'  => $e->getMessage()
    ]);
}
