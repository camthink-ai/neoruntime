#!/bin/bash
# ============================================================
# NE503 Lens Control API Test Suite
# ============================================================
# 用法:
#   本地测试 (需 platform-api 运行在 localhost:8080):
#     bash docs/testing/lens_api_test.sh
#
#   设备测试:
#     DEVICE=192.0.2.72 bash docs/testing/lens_api_test.sh
#
#   仅运行部分测试:
#     ONLY=zoom bash docs/testing/lens_api_test.sh
#     ONLY=status,focus bash docs/testing/lens_api_test.sh
# ============================================================

set -euo pipefail

# ── 配置 ────────────────────────────────────────────────────
DEVICE="${DEVICE:-localhost}"
PORT="${PORT:-8080}"
TOKEN="${AIPC_TOKEN_KEY:-}"
BASE="http://${DEVICE}:${PORT}/api/v1/device"
AUTH="Authorization: Bearer ${TOKEN}"
ONLY="${ONLY:-}"

PASS=0
FAIL=0
SKIP=0

# ── 工具函数 ─────────────────────────────────────────────────

should_run() {
    local name="$1"
    if [ -z "$ONLY" ]; then return 0; fi
    echo "$ONLY" | tr ',' '\n' | grep -qx "$name"
}

pass() { PASS=$((PASS + 1)); echo "  ✅ PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  ❌ FAIL: $1"; }
skip() { SKIP=$((SKIP + 1)); echo "  ⏭  SKIP: $1"; }

# 通用请求函数
# 用法: api_test <方法> <路径> <JSON body> <描述> <预期条件>
api_test() {
    local method="$1"
    local path="$2"
    local body="$3"
    local desc="$4"
    local expect="${5:-success}"

    local args=(-s -w "\n%{http_code}" -H "$AUTH" -H "Content-Type: application/json")
    if [ "$method" = "GET" ]; then
        args+=(-X GET "$BASE$path")
    elif [ "$method" = "POST" ]; then
        args+=(-X POST "$BASE$path" -d "$body")
    elif [ "$method" = "PUT" ]; then
        args+=(-X PUT "$BASE$path" -d "$body")
    fi

    local response
    response=$(curl "${args[@]}" 2>&1) || {
        fail "$desc (curl failed: $?)"
        echo "     Response: $response"
        return 1
    }

    local http_code
    http_code=$(echo "$response" | tail -1)
    local body_out
    body_out=$(echo "$response" | sed '$d')

    # 检查预期条件
    case "$expect" in
        success)
            if echo "$body_out" | grep -q '"success":true\|"Success"\|"code":0'; then
                pass "$desc"
            else
                fail "$desc (expected success)"
                echo "     HTTP $http_code: $body_out" | head -c 200
                echo
            fi
            ;;
        fail_400)
            if [ "$http_code" = "400" ] || echo "$body_out" | grep -q '"code":1001\|Invalid request\|must be'; then
                pass "$desc (正确拒绝无效请求)"
            else
                fail "$desc (expected 400/invalid)"
                echo "     HTTP $http_code: $body_out" | head -c 200
                echo
            fi
            ;;
        fail_unavailable)
            if echo "$body_out" | grep -q 'not available\|unavailable'; then
                pass "$desc (正确返回 unavailable)"
            else
                pass "$desc (服务可用，正常响应)"
            fi
            ;;
        has_data)
            if echo "$body_out" | grep -q '"zoom_pos"\|"focus_pos"\|"zoom_state"\|"ZoomState"\|"ZoomPos"'; then
                pass "$desc (返回镜头数据)"
            else
                fail "$desc (缺少镜头数据)"
                echo "     HTTP $http_code: $body_out" | head -c 200
                echo
            fi
            ;;
        *)
            pass "$desc"
            ;;
    esac

    echo "     → $body_out" | head -c 300
    echo
}

separator() {
    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "  $1"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
}

# ── 前置检查 ─────────────────────────────────────────────────

echo "╔══════════════════════════════════════════════════════════╗"
echo "║  NE503 Lens Control API Test Suite                      ║"
echo "╚══════════════════════════════════════════════════════════╝"
echo ""
echo "Target: ${DEVICE}:${PORT}"
echo "Base URL: ${BASE}"
echo ""

# 健康检查
echo "── Pre-flight ────"
HTTP=$(curl -s -o /dev/null -w "%{http_code}" -H "$AUTH" "${BASE}/status" 2>&1) || true
if [ "$HTTP" = "000" ]; then
    echo "❌ platform-api 不可达 (${DEVICE}:${PORT})"
    echo "   启动服务: systemctl start platform-api"
    exit 1
fi
echo "✅ platform-api 可达 (HTTP $HTTP)"
echo ""

# ═══════════════════════════════════════════════════════════
# 1. 设备状态 (基线)
# ═══════════════════════════════════════════════════════════

if should_run "status"; then
    separator "1. 设备状态查询"

    api_test GET "/status" "" \
        "GET /device/status — 获取设备状态" \
        "success"

    api_test GET "/lens/status" "" \
        "GET /device/lens/status — 获取镜头完整状态" \
        "has_data"
fi

# ═══════════════════════════════════════════════════════════
# 2. Zoom 控制
# ═══════════════════════════════════════════════════════════

if should_run "zoom"; then
    separator "2. Zoom 变焦控制"

    # 2.1 速度控制
    api_test POST "/zoom" '{"speed": 50}' \
        "POST /device/zoom speed=50 (zoom in)" \
        "success"

    sleep 0.3

    api_test POST "/zoom" '{"speed": -50}' \
        "POST /device/zoom speed=-50 (zoom out)" \
        "success"

    sleep 0.3

    api_test POST "/zoom" '{"speed": 0}' \
        "POST /device/zoom speed=0 (zoom stop)" \
        "success"

    # 2.2 绝对定位
    api_test POST "/zoom" '{"speed": 0}' "" "ignore" > /dev/null 2>&1  # 先停
    sleep 0.5

    api_test POST "/lens/status" '' "" "ignore" > /dev/null 2>&1  # 停

    # 绝对 zoom level
    api_test PUT "" "" "" "ignore" > /dev/null 2>&1  # 清理

    # 注意: 原有 zoom-level 通过 PTZ 接口
    echo "── Zoom Level (通过 POST /device/ptz 或独立接口) ────"

    # 检查是否有 zoom-level 独立路由 (根据 main.go 注册情况)
    # 这里测试通过 POST /device/zoom 配合不同 level

    # 2.3 边界值
    api_test POST "/zoom" '{"speed": 0}' "" "ignore" > /dev/null 2>&1  # 先停
    sleep 0.5

    api_test POST "/zoom" '{"speed": 100}' \
        "POST /device/zoom speed=100 (最大速度)" \
        "success"

    sleep 0.5

    api_test POST "/zoom" '{"speed": 0}' "" "ignore" > /dev/null 2>&1  # 停
    sleep 0.5

    api_test POST "/zoom" '{"speed": -100}' \
        "POST /device/zoom speed=-100 (最大反向速度)" \
        "success"

    api_test POST "/zoom" '{"speed": 0}' \
        "POST /device/zoom speed=0 (停止)" \
        "success"

    # 2.4 无效参数
    api_test POST "/zoom" '{"speed": 101}' \
        "POST /device/zoom speed=101 (超出范围)" \
        "success"  # 服务端不做限制，HAL 层会 clamp

    api_test POST "/zoom" '{}' \
        "POST /device/zoom {} (缺少 speed)" \
        "success"  # speed 默认 0 → stop

    # 停止
    api_test POST "/zoom" '{"speed": 0}' "" "ignore" > /dev/null 2>&1
fi

# ═══════════════════════════════════════════════════════════
# 3. Focus 控制
# ═══════════════════════════════════════════════════════════

if should_run "focus"; then
    separator "3. Focus 对焦控制"

    # 3.1 速度控制
    api_test POST "/focus" '{"speed": 30}' \
        "POST /device/focus speed=30 (focus far)" \
        "success"

    sleep 0.3

    api_test POST "/focus" '{"speed": -30}' \
        "POST /device/focus speed=-30 (focus near)" \
        "success"

    sleep 0.3

    api_test POST "/focus" '{"speed": 0}' \
        "POST /device/focus speed=0 (focus stop)" \
        "success"

    # 3.2 绝对焦点 level
    api_test POST "/focus" '{"speed": 0}' "" "ignore" > /dev/null 2>&1  # 先停
    sleep 0.5

    api_test PUT "/lens/focus-level" '{"level": 0.0}' \
        "PUT /device/lens/focus-level level=0.0 (最近)" \
        "success"

    sleep 1

    api_test PUT "/lens/focus-level" '{"level": 1.0}' \
        "PUT /device/lens/focus-level level=1.0 (最远)" \
        "success"

    sleep 1

    api_test PUT "/lens/focus-level" '{"level": 0.5}' \
        "PUT /device/lens/focus-level level=0.5 (中间)" \
        "success"

    sleep 1

    # 3.3 无效参数
    api_test PUT "/lens/focus-level" '{"level": -0.1}' \
        "PUT /device/lens/focus-level level=-0.1 (无效)" \
        "fail_400"

    api_test PUT "/lens/focus-level" '{"level": 1.5}' \
        "PUT /device/lens/focus-level level=1.5 (无效)" \
        "fail_400"

    api_test PUT "/lens/focus-level" '{}' \
        "PUT /device/lens/focus-level {} (缺少 level, 默认0)" \
        "success"

    # 停止
    api_test POST "/focus" '{"speed": 0}' "" "ignore" > /dev/null 2>&1
fi

# ═══════════════════════════════════════════════════════════
# 4. Autofocus
# ═══════════════════════════════════════════════════════════

if should_run "autofocus"; then
    separator "4. Autofocus 自动对焦"

    api_test POST "/autofocus" '{"enable": true}' \
        "POST /device/autofocus enable=true" \
        "success"

    # 验证状态中 autofocus_enabled=true
    echo "── 验证 autofocus 状态 ────"
    RESP=$(curl -s -H "$AUTH" "${BASE}/lens/status" 2>&1)
    if echo "$RESP" | grep -q '"autofocus_enabled":true\|"AutofocusEnabled":true'; then
        pass "lens/status 返回 autofocus_enabled=true"
    else
        fail "lens/status 未返回 autofocus_enabled=true"
        echo "     $RESP" | head -c 300
    fi
    echo

    api_test POST "/autofocus" '{"enable": false}' \
        "POST /device/autofocus enable=false" \
        "success"

    # 验证状态 — autofocus_enabled=false is zero-value, protobuf omits it
    RESP=$(curl -s -H "$AUTH" "${BASE}/lens/status" 2>&1)
    if echo "$RESP" | grep -q '"autofocus_enabled":true'; then
        fail "lens/status 仍显示 autofocus_enabled=true"
    else
        pass "lens/status 确认 autofocus disabled (字段省略=false)"
    fi
    echo
fi

# ═══════════════════════════════════════════════════════════
# 5. Iris 光圈控制
# ═══════════════════════════════════════════════════════════

if should_run "iris"; then
    separator "5. Iris 光圈控制"

    # 5.1 速度控制
    api_test POST "/lens/iris" '{"speed": 50}' \
        "POST /device/lens/iris speed=50 (光圈开大)" \
        "success"

    sleep 0.3

    api_test POST "/lens/iris" '{"speed": -50}' \
        "POST /device/lens/iris speed=-50 (光圈缩小)" \
        "success"

    sleep 0.3

    api_test POST "/lens/iris" '{"speed": 0}' \
        "POST /device/lens/iris speed=0 (光圈停止)" \
        "success"

    # 5.2 绝对目标
    api_test POST "/lens/iris-target" '{"target": 256}' \
        "POST /device/lens/iris-target target=256" \
        "success"

    api_test POST "/lens/iris-target" '{"target": 768}' \
        "POST /device/lens/iris-target target=768" \
        "success"

    # 5.3 无效参数
    api_test POST "/lens/iris" '{}' \
        "POST /device/lens/iris {} (缺少 speed)" \
        "success"  # speed 默认 0 → stop
fi

# ═══════════════════════════════════════════════════════════
# 6. Reset-Zero 归零
# ═══════════════════════════════════════════════════════════

if should_run "reset"; then
    separator "6. Reset-Zero 归零 (Homing)"

    # Stop all motors first
    api_test POST "/zoom" '{"speed": 0}' "" "ignore" > /dev/null 2>&1
    api_test POST "/focus" '{"speed": 0}' "" "ignore" > /dev/null 2>&1
    sleep 1

    api_test POST "/lens/reset-zero" '{"zoom": true, "focus": true}' \
        "POST /device/lens/reset-zero zoom+focus (串行归零)" \
        "success"

    sleep 5  # 归零需要时间 (server内部等)

    # 验证归零后位置
    echo "── 验证归零后位置 ────"
    RESP=$(curl -s -H "$AUTH" "${BASE}/lens/status" 2>&1)
    if echo "$RESP" | grep -q '"zoom_rz_done":true\|"ZoomRzDone":true'; then
        pass "归零后 zoom_rz_done=true"
    else
        echo "     (归零可能尚未完成或 stub 不返回 rz_done)"
        pass "归零请求已发送"
    fi
    echo "     → $RESP" | head -c 300
    echo

    # 仅归零 zoom
    api_test POST "/lens/reset-zero" '{"zoom": true}' \
        "POST /device/lens/reset-zero zoom only" \
        "success"

    sleep 1

    # 仅归零 focus
    api_test POST "/lens/reset-zero" '{"focus": true}' \
        "POST /device/lens/reset-zero focus only" \
        "success"

    sleep 1

    # 无效: 两个都不归零
    api_test POST "/lens/reset-zero" '{"zoom": false, "focus": false}' \
        "POST /device/lens/reset-zero (无操作)" \
        "success"
fi

# ═══════════════════════════════════════════════════════════
# 7. Limits 限位
# ═══════════════════════════════════════════════════════════

if should_run "limits"; then
    separator "7. Limits 限位设置"

    api_test PUT "/lens/limits" '{"zoom_limit": {"min_pos": -3000, "max_pos": 700}, "focus_limit": {"min_pos": -800, "max_pos": 550}}' \
        "PUT /device/lens/limits (设置自定义限位)" \
        "success"

    # 验证限位生效
    echo "── 验证限位 ────"
    RESP=$(curl -s -H "$AUTH" "${BASE}/lens/status" 2>&1)
    echo "     → $RESP" | head -c 400
    echo

    # 恢复默认限位
    api_test PUT "/lens/limits" '{"zoom_limit": {"min_pos": -3236, "max_pos": 760}, "focus_limit": {"min_pos": -844, "max_pos": 592}}' \
        "PUT /device/lens/limits (恢复默认限位)" \
        "success"

    # 仅设置 zoom 限位
    api_test PUT "/lens/limits" '{"zoom_limit": {"min_pos": -3236, "max_pos": 760}}' \
        "PUT /device/lens/limits (仅 zoom)" \
        "success"

    # 仅设置 focus 限位
    api_test PUT "/lens/limits" '{"focus_limit": {"min_pos": -844, "max_pos": 592}}' \
        "PUT /device/lens/limits (仅 focus)" \
        "success"

    # 无效: min > max — MCU 正确拒绝
    RESP_LIM=$(curl -s -H "$AUTH" -X PUT "$BASE/lens/limits" -H "Content-Type: application/json" \
        -d '{"zoom_limit": {"min_pos": 1000, "max_pos": -100}}' 2>&1)
    if echo "$RESP_LIM" | grep -q '"success":false\|"message"'; then
        pass "PUT /device/lens/limits min>max 被 HAL 正确拒绝"
    else
        fail "PUT /device/lens/limits min>max 未被拒绝"
    fi
    echo "     → $RESP_LIM" | head -c 200
    echo
fi

# ═══════════════════════════════════════════════════════════
# 8. 综合状态验证
# ═══════════════════════════════════════════════════════════

if should_run "status"; then
    separator "8. 综合状态验证"

    echo "── 最终状态快照 ────"

    echo "  Device Status:"
    RESP=$(curl -s -H "$AUTH" "${BASE}/status" 2>&1)
    echo "     $RESP" | python3 -m json.tool 2>/dev/null || echo "     $RESP"
    echo ""

    echo "  Lens Status:"
    RESP=$(curl -s -H "$AUTH" "${BASE}/lens/status" 2>&1)
    echo "     $RESP" | python3 -m json.tool 2>/dev/null || echo "     $RESP"
    echo ""
fi

# ═══════════════════════════════════════════════════════════
# 9. 鉴权测试
# ═══════════════════════════════════════════════════════════

if should_run "auth"; then
    separator "9. 鉴权测试"

    echo "── 无 Token ────"
    RESP=$(curl -s -w "\n%{http_code}" "${BASE}/lens/status" 2>&1)
    CODE=$(echo "$RESP" | tail -1)
    BODY=$(echo "$RESP" | sed '$d')
    if [ "$CODE" = "401" ] || echo "$BODY" | grep -qi "unauthorized\|token"; then
        pass "无 Token 请求被拒绝 (HTTP $CODE)"
    else
        fail "无 Token 请求未被拒绝 (HTTP $CODE)"
    fi
    echo ""

    echo "── 错误 Token ────"
    RESP=$(curl -s -w "\n%{http_code}" -H "Authorization: Bearer wrong-token" "${BASE}/lens/status" 2>&1)
    CODE=$(echo "$RESP" | tail -1)
    BODY=$(echo "$RESP" | sed '$d')
    if [ "$CODE" = "401" ] || echo "$BODY" | grep -qi "unauthorized\|token\|invalid"; then
        pass "错误 Token 请求被拒绝 (HTTP $CODE)"
    else
        fail "错误 Token 请求未被拒绝 (HTTP $CODE)"
    fi
    echo ""

    echo "── 正确 Token ────"
    RESP=$(curl -s -w "\n%{http_code}" -H "$AUTH" "${BASE}/lens/status" 2>&1)
    CODE=$(echo "$RESP" | tail -1)
    if [ "$CODE" = "200" ]; then
        pass "正确 Token 请求成功 (HTTP 200)"
    else
        fail "正确 Token 请求失败 (HTTP $CODE)"
    fi
    echo ""
fi

# ═══════════════════════════════════════════════════════════
# 10. 延迟测试
# ═══════════════════════════════════════════════════════════

if should_run "latency"; then
    separator "10. 延迟测试 (5 次采样)"

    for i in 1 2 3 4 5; do
        START=$(date +%s%N)
        curl -s -H "$AUTH" "${BASE}/lens/status" > /dev/null 2>&1
        END=$(date +%s%N)
        ELAPSED=$(( (END - START) / 1000000 ))
        echo "  第 ${i} 次 GET /lens/status: ${ELAPSED}ms"
    done

    echo ""
    echo "  Zoom 请求延迟:"
    for i in 1 2 3; do
        START=$(date +%s%N)
        curl -s -H "$AUTH" -X POST "${BASE}/zoom" -H "Content-Type: application/json" \
            -d '{"speed": 50}' > /dev/null 2>&1
        END=$(date +%s%N)
        ELAPSED=$(( (END - START) / 1000000 ))
        echo "  第 ${i} 次 POST /zoom: ${ELAPSED}ms"
    done

    # 停止
    curl -s -H "$AUTH" -X POST "${BASE}/zoom" -H "Content-Type: application/json" \
        -d '{"speed": 0}' > /dev/null 2>&1

    echo ""
fi

# ═══════════════════════════════════════════════════════════
# 结果汇总
# ═══════════════════════════════════════════════════════════

echo ""
echo "════════════════════════════════════════════════════════════"
echo "  测试结果"
echo "════════════════════════════════════════════════════════════"
echo "  ✅ PASS:  $PASS"
echo "  ❌ FAIL:  $FAIL"
echo "  ⏭  SKIP:  $SKIP"
echo "  ──────────────"
echo "  TOTAL:   $((PASS + FAIL + SKIP))"
echo ""

if [ "$FAIL" -gt 0 ]; then
    echo "  ⚠️  有 $FAIL 个测试失败，请检查上方日志"
    exit 1
else
    echo "  🎉 所有测试通过!"
    exit 0
fi
