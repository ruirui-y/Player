// ============================================================
// 全局状态
// ============================================================

const HISTORY_SIZE = 60;            // 60 个点 × 2s = 2 分钟
let history = [];                    // 快照历史
const COUNTER_NAMES = [             // 核心计数器，用于顶部卡片
    'create_order_total',
    'pay_success_total',
    'pay_failed_total'
];

// ============================================================
// 工具函数
// ============================================================

function getCounterValue(data, name) {
    const found = data.counters.find(c => c.name === name);
    return found ? found.value : 0;
}

function getFirstHistogram(data) {
    return data.histograms.length > 0 ? data.histograms[0] : null;
}

// ============================================================
// 更新统计卡片
// ============================================================

function updateCards(data) {
    document.getElementById('createOrderTotal').textContent =
        getCounterValue(data, 'create_order_total');
    document.getElementById('paySuccessTotal').textContent =
        getCounterValue(data, 'pay_success_total');
    document.getElementById('payFailedTotal').textContent =
        getCounterValue(data, 'pay_failed_total');

    const hist = getFirstHistogram(data);
    document.getElementById('avgMs').textContent =
        hist ? hist.avg_ms + 'ms' : '0ms';
}

// ============================================================
// QPS 折线图（Canvas）
// ============================================================

function drawQpsChart() {
    const canvas = document.getElementById('qpsChart');
    const ctx = canvas.getContext('2d');
    const w = canvas.width;
    const h = canvas.height;

    ctx.clearRect(0, 0, w, h);

    // 需要至少 2 个快照才能算差值
    if (history.length < 2) {
        ctx.fillStyle = '#aaa';
        ctx.font = '14px sans-serif';
        ctx.textAlign = 'center';
        ctx.fillText('等待更多数据...', w / 2, h / 2);
        return;
    }

    // 计算 QPS 序列：(当前值 - 前一个值) / 2秒
    const qpsList = [];
    for (let i = 1; i < history.length; i++) {
        const prev = history[i - 1].counters.find(
            c => c.name === 'create_order_total');
        const curr = history[i].counters.find(
            c => c.name === 'create_order_total');
        if (prev && curr) {
            const diff = Math.max(0, curr.value - prev.value);
            qpsList.push(diff / 2);
        }
    }

    if (qpsList.length === 0) return;

    const maxQps = Math.max(...qpsList, 1);
    const padding = { top: 15, right: 15, bottom: 25, left: 45 };
    const plotW = w - padding.left - padding.right;
    const plotH = h - padding.top - padding.bottom;

    // 画网格线
    ctx.strokeStyle = '#eee';
    ctx.lineWidth = 1;
    for (let i = 0; i <= 4; i++) {
        const y = padding.top + (plotH * (1 - i / 4));
        ctx.beginPath();
        ctx.moveTo(padding.left, y);
        ctx.lineTo(w - padding.right, y);
        ctx.stroke();

        // Y 轴标签
        ctx.fillStyle = '#888';
        ctx.font = '12px monospace';
        ctx.textAlign = 'right';
        ctx.fillText(Math.round(maxQps * i / 4), padding.left - 8, y + 4);
    }

    // 画折线
    const stepX = plotW / Math.max(qpsList.length - 1, 1);
    ctx.beginPath();
    ctx.strokeStyle = '#2196F3';
    ctx.lineWidth = 2;
    ctx.lineJoin = 'round';

    for (let i = 0; i < qpsList.length; i++) {
        const x = padding.left + i * stepX;
        const y = padding.top + plotH * (1 - qpsList[i] / maxQps);
        if (i === 0) {
            ctx.moveTo(x, y);
        }
        else {
            ctx.lineTo(x, y);
        }
    }
    ctx.stroke();

    // 填充渐变
    const lastX = padding.left + (qpsList.length - 1) * stepX;
    ctx.lineTo(lastX, padding.top + plotH);
    ctx.lineTo(padding.left, padding.top + plotH);
    ctx.closePath();
    ctx.fillStyle = 'rgba(33, 150, 243, 0.08)';
    ctx.fill();
}

// ============================================================
// 响应时间柱状图（CSS）
// ============================================================

function drawBarChart(hist) {
    const container = document.getElementById('barChart');
    container.innerHTML = '';

    if (!hist || !hist.buckets || hist.buckets.length === 0) {
        container.innerHTML = '<p style="color:#aaa;">暂无数据</p>';
        return;
    }

    // 找最大 count 用于计算柱高
    const maxCount = Math.max(...hist.buckets.map(b => b.count), 1);
    const maxBarHeight = 140;

    hist.buckets.forEach(bucket => {
        const wrapper = document.createElement('div');
        wrapper.className = 'bar-wrapper';

        const countLabel = document.createElement('div');
        countLabel.className = 'bar-count';
        countLabel.textContent = bucket.count;

        const bar = document.createElement('div');
        bar.className = 'bar';
        const ratio = bucket.count / maxCount;
        bar.style.height = Math.max(ratio * maxBarHeight, 4) + 'px';

        const label = document.createElement('div');
        label.className = 'bar-label';
        label.textContent = bucket.le === '+Inf' ? '+Inf' : '≤' + bucket.le;

        wrapper.appendChild(countLabel);
        wrapper.appendChild(bar);
        wrapper.appendChild(label);
        container.appendChild(wrapper);
    });
}

// ============================================================
// 指标明细表
// ============================================================

function updateTable(data) {
    const tbody = document.getElementById('metricBody');
    tbody.innerHTML = '';

    // Counters
    data.counters.forEach(c => {
        const tr = document.createElement('tr');
        tr.innerHTML = `
            <td>${c.name}</td>
            <td>${c.help}</td>
            <td>${c.value}</td>
        `;
        tbody.appendChild(tr);
    });

    // 空行分隔
    if (data.counters.length > 0 && data.histograms.length > 0) {
        const sep = document.createElement('tr');
        sep.innerHTML = '<td colspan="3" style="padding:4px;"></td>';
        tbody.appendChild(sep);
    }

    // Histograms（显示总次数和平均耗时）
    data.histograms.forEach(h => {
        const tr = document.createElement('tr');
        tr.innerHTML = `
            <td>${h.name}</td>
            <td>${h.help}</td>
            <td>总计 ${h.total} 次，平均 ${h.avg_ms}ms</td>
        `;
        tbody.appendChild(tr);
    });
}

// ============================================================
// 主刷新循环
// ============================================================

async function refresh() {
    try {
        const resp = await fetch('/api/metrics');
        if (!resp.ok) throw new Error('HTTP ' + resp.status);
        const data = await resp.json();

        // 更新时间
        document.getElementById('updateTime').textContent =
            '更新于: ' + new Date().toLocaleTimeString();

        // 更新卡片
        updateCards(data);

        // 维护历史快照
        const snapshot = {
            time: Date.now(),
            counters: JSON.parse(JSON.stringify(data.counters)),
            histograms: JSON.parse(JSON.stringify(data.histograms))
        };
        history.push(snapshot);
        if (history.length > HISTORY_SIZE) history.shift();

        // 画图
        drawQpsChart();
        drawBarChart(getFirstHistogram(data));

        // 更新明细表
        updateTable(data);
    }
    catch (err) {
        document.getElementById('updateTime').textContent =
            '❌ 连接失败: ' + err.message;
    }
}

// ============================================================
// 启动
// ============================================================

// 立即刷一次，然后每 2 秒刷新
refresh();
setInterval(refresh, 2000);