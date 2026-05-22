// WiFi Repeater Dashboard JavaScript
// Handles fetching network and monitoring data, rendering UI, and user interactions

var dnsExpanded = false;
var updateInterval = 2000;
var statsData = null; // Store latest stats
var dashboardAuthenticated = false; // Track if user has logged in this session

// Utility Functions
function formatRSSI(rssi) {
    if (rssi >= -50) return { text: 'Excellent', color: 'status-good', percent: 100 };
    if (rssi >= -60) return { text: 'Very Good', color: 'status-good', percent: 80 };
    if (rssi >= -70) return { text: 'Good', color: 'status-good', percent: 60 };
    if (rssi >= -80) return { text: 'Fair', color: 'status-warning', percent: 40 };
    return { text: 'Weak', color: 'status-bad', percent: 20 };
}

function formatUptime(seconds) {
    var total = Number(seconds) || 0;
    if (total < 0) total = 0;
    var h = Math.floor(total / 3600);
    var m = Math.floor ((total % 3600) / 60);
    var s = Math.floor(total % 60);
    var parts = [];
    if (h > 0) parts.push(h + 'h');
    parts.push(m + 'm');
    parts.push(s + 's');
    return parts.join(' ');
}

function formatBytes(bytes) {
    if (bytes === 0) return '0 B';
    var k = 1024;
    var sizes = ['B', 'KB', 'MB', 'GB'];
    var i = Math.floor(Math.log(bytes) / Math.log(k));
    return (bytes / Math.pow(k, i)).toFixed(1) + ' ' + sizes[i];
}

function formatBytesPerSec(bytesPerSec) {
    if (bytesPerSec === 0) return '0 B/s';
    var k = 1024;
    var sizes = ['B/s', 'KB/s', 'MB/s'];
    var i = Math.floor(Math.log(bytesPerSec) / Math.log(k));
    return (bytesPerSec / Math.pow(k, i)).toFixed(2) + ' ' + sizes[i];
}

// AJAX Helper
function requestJson(url, onSuccess, onError) {
    if (window.fetch) {
        fetch(url, { cache: 'no-store' })
            .then(function(response) {
                if (!response.ok) {
                    throw new Error('HTTP ' + response.status);
                }
                return response.json();
            })
            .then(onSuccess)
            .catch(onError);
        return;
    }
    var xhr = new XMLHttpRequest();
    xhr.open('GET', url, true);
    xhr.onreadystatechange = function() {
        if (xhr.readyState !== 4) return;
        if (xhr.status >= 200 && xhr.status < 300) {
            try {
                var data = JSON.parse(xhr.responseText);
                onSuccess(data);
            } catch (err) {
                onError(err);
            }
        } else {
            onError(new Error('HTTP ' + xhr.status));
        }
    };
    xhr.send(null);
}

// Data Normalization
function normalizeData(data) {
    if (!data) return null;
    if (!data.rssi_info) {
        var rssiValue = typeof data.rssi === 'number' ? data.rssi : -100;
        data.rssi_info = formatRSSI(rssiValue);
    }
    if (!data.client_list || !data.client_list.length) {
        data.client_list = [];
    }
    if (!data.dns_logs || !data.dns_logs.length) {
        data.dns_logs = [];
    }
    return data;
}

// Render Functions
function renderError(message) {
    var dashboard = document.getElementById('dashboard');
    if (!dashboard) return;
    dashboard.innerHTML = '<div class="card"><div class="loading">Error: ' + message + '</div></div>';
    var lastUpdate = document.getElementById('lastUpdate');
    if (lastUpdate) lastUpdate.textContent = '-';
}

function renderMonitoringCards() {
    if (!statsData) return '';
    
    var html = '';
    
    // CPU Card
    var cpuPercent = statsData.cpu.total_percent || 0;
    var cpuColor = cpuPercent > 80 ? 'status-bad' : (cpuPercent > 50 ? 'status-warning' : 'status-good');
    html += '<div class="card"><h2>💻 CPU Usage</h2>';
    html += '<div class="info-row"><span class="label">Total CPU</span><span class="value ' + cpuColor + '">' + cpuPercent + '%</span></div>';
    html += '<div class="progress-bar"><div class="progress-fill progress-cpu" style="width: ' + cpuPercent + '%">' + cpuPercent + '%</div></div>';
    html += '<div class="info-row"><span class="label">Active Tasks</span><span class="value">' + (statsData.cpu.task_count || 0) + '</span></div>';
    html += '</div>';
    
    // Memory Card
    var memPercent = statsData.mem.heap_usage_percent || 0;
    var memColor = memPercent > 80 ? 'status-bad' : (memPercent > 60 ? 'status-warning' : 'status-good');
    html += '<div class="card"><h2>🧠 Memory</h2>';
    html += '<div class="info-row"><span class="label">Usage</span><span class="value ' + memColor + '">' + memPercent.toFixed(1) + '%</span></div>';
    html += '<div class="progress-bar"><div class="progress-fill progress-mem" style="width: ' + memPercent + '%">' + memPercent.toFixed(1) + '%</div></div>';
    html += '<div class="metric-grid">';
    html += '<div class="metric-box"><div class="metric-value">' + formatBytes(statsData.mem.free_heap) + '</div><div class="metric-label">Free</div></div>';
    html += '<div class="metric-box"><div class="metric-value">' + formatBytes(statsData.mem.total_heap) + '</div><div class="metric-label">Total</div></div>';
    html += '</div>';
    html += '</div>';
    
    // Network Throughput Card
    var txRate = statsData.net.tx_bytes_per_sec || 0;
    var rxRate = statsData.net.rx_bytes_per_sec || 0;
    html += '<div class="card"><h2>📊 Network Throughput</h2>';
    html += '<div class="metric-grid">';
    html += '<div class="metric-box"><div class="metric-value throughput-value">' + formatBytesPerSec(txRate) + '</div><div class="metric-label">TX Rate</div></div>';
    html += '<div class="metric-box"><div class="metric-value throughput-value">' + formatBytesPerSec(rxRate) + '</div><div class="metric-label">RX Rate</div></div>';
    html += '</div>';
    html += '<div class="info-row"><span class="label">TX Total</span><span class="value">' + formatBytes(statsData.net.tx_bytes) + '</span></div>';
    html += '<div class="info-row"><span class="label">RX Total</span><span class="value">' + formatBytes(statsData.net.rx_bytes) + '</span></div>';
    html += '</div>';
    
    // DNS Stats Card
    var dnsHits = (statsData.bench && statsData.bench.dns_cache_hits) || 0;
    var dnsMisses = (statsData.bench && statsData.bench.dns_cache_misses) || 0;
    var totalDns = dnsHits + dnsMisses;
    var hitRate = totalDns > 0 ? ((dnsHits / totalDns) * 100).toFixed(1) : 0;

    html += '<div class="card"><h2>⚡ DNS Performance</h2>';
    html += '<div class="metric-grid">';
    html += '<div class="metric-box"><div class="metric-value status-good">' + hitRate + '%</div><div class="metric-label">Cache Hit Rate</div></div>';
    html += '<div class="metric-box"><div class="metric-value">' + ((statsData.bench.dns_max_latency_us || 0) / 1000).toFixed(0) + 'ms</div><div class="metric-label">Max Latency</div></div>';
    html += '</div>';
    html += '<div class="info-row"><span class="label">Cache Hits</span><span class="value status-good">' + dnsHits + '</span></div>';
    html += '<div class="info-row"><span class="label">Cache Misses</span><span class="value">' + dnsMisses + '</span></div>';
    html += '<div class="info-row"><span class="label">Avg Latency</span><span class="value">' + ((statsData.bench.dns_avg_latency_us || 0) / 1000).toFixed(1) + ' ms</span></div>';
    html += '</div>';

    return html;
}

function renderDashboard(data) {
    var dashboard = document.getElementById('dashboard');
    if (!dashboard) return;
    
    dashboard.innerHTML = '';
    
    // STA Card
    var staHTML = '<div class="card"><h2>⬆️ Uplink (STA)</h2>';
    staHTML += '<div class="info-row"><span class="label">Status</span><span class="value">';
    staHTML += data.sta_connected ? '<span class="badge badge-connected">Connected</span>' : '<span class="badge badge-disconnected">Disconnected</span>';
    staHTML += '</span></div>';
    if (data.sta_connected) {
        staHTML += '<div class="info-row"><span class="label">SSID</span><span class="value">' + data.sta_ssid + '</span></div>';
        staHTML += '<div class="info-row"><span class="label">IP Address</span><span class="value">' + data.sta_ip + '</span></div>';
        staHTML += '<div class="info-row"><span class="label">Netmask</span><span class="value">' + data.sta_netmask + '</span></div>';
        staHTML += '<div class="info-row"><span class="label">Gateway</span><span class="value">' + data.sta_gw + '</span></div>';
        staHTML += '<div class="info-row"><span class="label">Channel</span><span class="value">' + data.sta_channel + '</span></div>';
        staHTML += '<div class="info-row"><span class="label">Band</span><span class="value">' + data.sta_band + '</span></div>';
        staHTML += '<div class="info-row"><span class="label">Signal Strength</span>';
        staHTML += '<span class="value ' + data.rssi_info.color + '">' + data.rssi_info.text + ' (' + data.rssi + ' dBm)</span>';
        staHTML += '<div class="rssi-bar"><div class="rssi-fill" style="width: ' + data.rssi_info.percent + '%"></div></div>';
        staHTML += '</div>';
    }
    staHTML += '</div>';
    
    // AP Card
    var apHTML = '<div class="card"><h2>⬇️ Access Point (AP)</h2>';
    apHTML += '<div class="info-row"><span class="label">SSID</span><span class="value">' + data.ap_ssid + '</span></div>';
    apHTML += '<div class="info-row"><span class="label">IP Address</span><span class="value">' + data.ap_ip + '</span></div>';
    apHTML += '<div class="info-row"><span class="label">Netmask</span><span class="value">' + data.ap_netmask + '</span></div>';
    apHTML += '<div class="info-row"><span class="label">Gateway</span><span class="value">' + data.ap_gw + '</span></div>';
    apHTML += '<div class="info-row"><span class="label">Connected Clients</span><span class="value status-good">' + data.ap_clients + '</span></div>';
    if (data.ap_clients > 0 && data.client_list.length) {
        apHTML += '<div class="clients-list">';
        for (var i = 0; i < data.client_list.length; i++) {
            var entry = data.client_list[i] || {};
            var mac = entry.mac ? entry.mac : 'unknown';
            var ipv4 = entry.ipv4 ? entry.ipv4 : '-';
            apHTML += '<div class="client-item">' + mac;
            apHTML += ' <div class="client-ipv4">IPv4: ' + ipv4 + '</div>';
            apHTML += '</div>';
        }
        apHTML += '</div>';
    }
    apHTML += '</div>';
    
    // Features Card
    var featuresHTML = '<div class="card"><h2>⚙️ Features & Power</h2>';
    featuresHTML += '<div class="info-row"><span class="label">NAT Enabled</span><span class="value">';
    featuresHTML += data.nat_enabled ? '<span class="status-good">Yes</span>' : '<span class="status-bad">No</span>';
    featuresHTML += '</span></div>';
    featuresHTML += '<div class="info-row"><span class="label">Monitoring</span><span class="value status-good">Active</span></div>';
    featuresHTML += '<div class="info-row"><span class="label">Power Mode</span><span class="value">' + data.power_mode + '</span></div>';
    featuresHTML += '<div class="info-row"><span class="label">Estimated Power</span><span class="value status-warning">' + data.estimated_power + '</span></div>';
    featuresHTML += '<div class="info-row"><span class="label">Uptime</span><span class="value">' + data.uptime + '</span></div>';
    featuresHTML += '</div>';
    
    // DNS Card
    var dnsHTML = '<div class="card"><h2>🧭 DNS Activity</h2>';
    if (data.dns_logs.length) {
        dnsHTML += '<div class="dns-list">';
        var maxDns = dnsExpanded ? data.dns_logs.length : Math.min(4, data.dns_logs.length);
        for (var i = 0; i < maxDns; i++) {
            var log = data.dns_logs[i] || {};
            var mac = log.mac || 'unknown';
            var ip = log.ip || '-';
            var domain = log.domain || '-';
            var qtype = (typeof log.qtype === 'number') ? log.qtype : '-';
            var ts = formatUptime(log.ts);
            dnsHTML += '<div class="dns-item' + (i === 0 ? ' latest' : '') + '">';
            dnsHTML += '<div class="dns-meta">' + ts + ' | ' + mac + ' | ' + ip + ' | type ' + qtype + '</div>';
            dnsHTML += '<div class="dns-domain">' + domain + '</div>';
            dnsHTML += '</div>';
        }
        dnsHTML += '</div>';
        if (data.dns_logs.length > 4) {
            dnsHTML += '<div class="dns-toggle"><button onclick="toggleDnsLogs()">' + (dnsExpanded ? 'Show less' : 'Show more') + '</button></div>';
        }
    } else {
        dnsHTML += '<div class="loading">No DNS activity yet</div>';
    }
    dnsHTML += '</div>';
    
    // Render monitoring cards if stats available
    var monitoringHTML = renderMonitoringCards();
    
    // Combine all cards
    dashboard.innerHTML = staHTML + apHTML + monitoringHTML + featuresHTML + dnsHTML;
    
    // Update timestamp
    var lastUpdate = document.getElementById('lastUpdate');
    if (lastUpdate) {
        lastUpdate.textContent = new Date().toLocaleTimeString();
    }
}

// Data Fetching
function fetchStats() {
    requestJson('/api/stats', function(data) {
        statsData = data;
        // Stats fetched, will be used when rendering network data
    }, function(error) {
        // Silent fail for stats - not critical
        if (window.console && console.warn) {
            console.warn('Failed to fetch stats:', error);
        }
    });
}

function fetchData() {
    // Fetch stats first (async)
    fetchStats();
    
    // Fetch network data
    requestJson('/api/network', function(data) {
        data = normalizeData(data);
        if (!data) {
            renderError('No data');
            return;
        }
        renderDashboard(data);
    }, function(error) {
        var message = (error && error.message) ? error.message : 'Request failed';
        renderError(message);
        if (window.console && console.error) {
            console.error('Error fetching data:', error);
        }
    });
}

function toggleDnsLogs() {
    dnsExpanded = !dnsExpanded;
    fetchData();
}

// Shutdown Modal Functions
function showShutdownModal() {
    document.getElementById('shutdownModal').style.display = 'block';
}

function closeShutdownModal() {
    document.getElementById('shutdownModal').style.display = 'none';
}

function confirmShutdown() {
    closeShutdownModal();
    if (!window.fetch) {
        alert('Error: Shutdown requires a modern browser');
        return;
    }
    fetch('/api/shutdown', { 
        method: 'POST', 
        headers: { 'Content-Type': 'application/json' }, 
        body: JSON.stringify({ confirm: true }) 
    })
    .then(function(response) {
        if (response.ok) {
            alert('Device shutting down. It will enter deep sleep and can be restarted by pressing the reset button.');
        } else {
            alert('Error: Failed to shutdown device');
        }
    })
    .catch(function(error) {
        if (window.console && console.error) {
            console.error('Error:', error);
        }
        alert('Error: Could not reach shutdown endpoint');
    });
}

// Login Modal Functions
function openLoginModal() {
    document.getElementById('loginError').style.display = 'none';
    document.getElementById('login_user').value = '';
    document.getElementById('login_pass').value = '';
    document.getElementById('loginModal').style.display = 'block';
    // Auto-focus username field
    setTimeout(function() { document.getElementById('login_user').focus(); }, 100);
}

function closeLoginModal() {
    document.getElementById('loginModal').style.display = 'none';
}

function submitLogin() {
    var username = document.getElementById('login_user').value.trim();
    var password = document.getElementById('login_pass').value;

    if (!username || !password) {
        document.getElementById('loginError').style.display = 'block';
        document.getElementById('loginError').textContent = '❌ Please enter both username and password.';
        return;
    }

    var btn = document.getElementById('loginSubmitBtn');
    btn.disabled = true;
    btn.textContent = 'Verifying...';

    fetch('/api/auth', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ username: username, password: password })
    })
    .then(function(response) { return response.json(); })
    .then(function(data) {
        btn.disabled = false;
        btn.textContent = 'Login';
        if (data.ok) {
            dashboardAuthenticated = true;
            closeLoginModal();
            openSettingsPanel();
        } else {
            document.getElementById('loginError').style.display = 'block';
            document.getElementById('loginError').textContent = '❌ Invalid username or password.';
        }
    })
    .catch(function(err) {
        btn.disabled = false;
        btn.textContent = 'Login';
        document.getElementById('loginError').style.display = 'block';
        document.getElementById('loginError').textContent = '❌ Connection error. Try again.';
    });
}

// Settings Functions
function openSettings() {
    if (!dashboardAuthenticated) {
        openLoginModal();
        return;
    }
    openSettingsPanel();
}

function openSettingsPanel() {
    document.getElementById('settingsModal').style.display = 'block';
    
    // Load current settings
    requestJson('/api/config', function(data) {
        document.getElementById('sta_ssid').value = data.sta_ssid || '';
        document.getElementById('sta_pass').value = ''; // Don't show password
        document.getElementById('ap_ssid').value = data.ap_ssid || '';
        document.getElementById('ap_pass').value = ''; // Don't show password
        document.getElementById('ap_max_conn').value = data.ap_max_conn || 4;

        // Populate SSID history datalist
        var datalist = document.getElementById('ssid_history_list');
        if (datalist && data.ssid_history && data.ssid_history.length) {
            datalist.innerHTML = '';
            for (var i = 0; i < data.ssid_history.length; i++) {
                var option = document.createElement('option');
                option.value = data.ssid_history[i];
                datalist.appendChild(option);
            }
        }
    }, function(err) {
        alert('Failed to load settings: ' + err.message);
    });
}

function closeSettings() {
    document.getElementById('settingsModal').style.display = 'none';
}

function saveSettings() {
    var config = {
        sta_ssid: document.getElementById('sta_ssid').value,
        sta_pass: document.getElementById('sta_pass').value,
        ap_ssid: document.getElementById('ap_ssid').value,
        ap_pass: document.getElementById('ap_pass').value,
        ap_max_conn: parseInt(document.getElementById('ap_max_conn').value, 10)
    };
    
    // Basic validation
    if (!config.sta_ssid) {
        alert('Uplink SSID is required');
        return;
    }
    if (!config.ap_ssid) {
        alert('Repeater SSID is required');
        return;
    }
    if (config.ap_pass && config.ap_pass.length < 8) {
        alert('AP Password must be at least 8 characters');
        return;
    }
    
    if (!confirm('Save settings and restart device?')) {
        return;
    }
    
    fetch('/api/config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(config)
    })
    .then(function(response) {
        if (!response.ok) throw new Error('Save failed');
        return response.json();
    })
    .then(function(data) {
        closeSettings();
        // Trigger restart
        return fetch('/api/restart', { method: 'POST' });
    })
    .then(function() {
        alert('Settings saved. Device is restarting...');
        // Reload page after a delay
        setTimeout(function() { location.reload(); }, 10000);
    })
    .catch(function(err) {
        alert('Error saving settings: ' + err.message);
    });
}

// Reset to Defaults Functions
function showResetModal() {
    document.getElementById('settingsModal').style.display = 'none';
    document.getElementById('resetModal').style.display = 'block';
}

function closeResetModal() {
    document.getElementById('resetModal').style.display = 'none';
    document.getElementById('settingsModal').style.display = 'block';
}

function confirmReset() {
    document.getElementById('resetModal').style.display = 'none';
    fetch('/api/config/reset', { method: 'POST' })
        .then(function(response) {
            if (!response.ok) throw new Error('HTTP ' + response.status);
            return response.json();
        })
        .then(function(data) {
            alert('Defaults restored. Device is restarting...\nReconnect to the repeater AP after ~10 seconds.');
            setTimeout(function() { location.reload(); }, 12000);
        })
        .catch(function(err) {
            alert('Error resetting to defaults: ' + err.message);
        });
}

// Modal click-outside-to-close
window.onclick = function(event) {
    var modal = document.getElementById('shutdownModal');
    var settingsModal = document.getElementById('settingsModal');
    var resetModal = document.getElementById('resetModal');
    var loginModal = document.getElementById('loginModal');
    if (event.target === modal) {
        closeShutdownModal();
    }
    if (event.target === settingsModal) {
        closeSettings();
    }
    if (event.target === resetModal) {
        closeResetModal();
    }
    if (event.target === loginModal) {
        closeLoginModal();
    }
};

// Allow Enter key to submit login
document.addEventListener('DOMContentLoaded', function() {
    var loginPass = document.getElementById('login_pass');
    if (loginPass) {
        loginPass.addEventListener('keydown', function(e) {
            if (e.key === 'Enter') { submitLogin(); }
        });
    }
    var loginUser = document.getElementById('login_user');
    if (loginUser) {
        loginUser.addEventListener('keydown', function(e) {
            if (e.key === 'Enter') { document.getElementById('login_pass').focus(); }
        });
    }
});

// Initialize
fetchData();
setInterval(fetchData, updateInterval);
