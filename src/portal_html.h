#pragma once

const char PORTAL_HTML[] PROGMEM = R"=====(<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Kinetic Switch Config</title>
  <style>
    body { font-family: sans-serif; max-width: 640px; margin: 0 auto; padding: 1em; }
    h2 { border-bottom: 1px solid #ccc; padding-bottom: 4px; }
    label { display: block; margin-bottom: 8px; }
    label span { display: inline-block; width: 80px; }
    input[type=text], input[type=password], select { padding: 4px 6px; width: 100%; box-sizing: border-box; }
    table { width: 100%; border-collapse: collapse; margin-bottom: 1em; }
    td, th { padding: 4px 8px; border: 1px solid #ddd; text-align: left; }
    th { background: #f5f5f5; }
    .row { display: flex; gap: 6px; margin-bottom: 6px; }
    .row input { flex: 1; }
    button { padding: 6px 12px; cursor: pointer; }
    #save-btn { background: #2a7; color: #fff; border: none; padding: 10px 20px; font-size: 1em; margin-top: 1em; }
    #status { margin-top: 8px; color: green; }
    code { background: #f0f0f0; padding: 2px 4px; border-radius: 2px; font-size: 0.9em; }
  </style>
</head>
<body>
  <h1>Kinetic Switch Config</h1>

  <h2>WiFi</h2>
  <label><span>Network</span>
    <select id="ssid_sel" onchange="onSelChange()">
      <option value="">— type custom below —</option>
      {{WIFI_OPTIONS}}
    </select>
  </label>
  <label><span>SSID</span><input type="text" id="ssid" value="{{SSID}}" /></label>
  <label><span>Password</span><input type="password" id="pass" value="{{PASS}}" /></label>

  <h2>Switches</h2>
  <table>
    <thead><tr><th>Name</th><th>Pattern</th><th>URL</th><th></th></tr></thead>
    <tbody id="sw_body"></tbody>
  </table>

  <h3>Add Switch</h3>
  <div class="row">
    <input type="text" id="new_name" placeholder="Name (e.g. Kitchen Light)" />
  </div>
  <div class="row">
    <input type="text" id="new_pat" placeholder="Pattern (e.g. 8e8e88e88888)" />
    <button id="learn-btn" onclick="learnPattern()">Learn</button>
  </div>
  <div id="learn-status" style="font-size:0.85em;color:#888;margin-bottom:6px;"></div>
  <div class="row">
    <input type="text" id="new_url" placeholder="URL to call on press" />
  </div>
  <div class="row">
    <button onclick="addSwitch()">Add</button>
  </div>

  <br>
  <button id="save-btn" onclick="save()">Save &amp; Restart</button>
  <div id="status"></div>

  <script>
    let switches = {{SWITCHES_JSON}};

    function render() {
      const tbody = document.getElementById('sw_body');
      if (!switches.length) {
        tbody.innerHTML = '<tr><td colspan="4"><em>No switches configured</em></td></tr>';
        return;
      }
      tbody.innerHTML = switches.map((sw, i) =>
        `<tr><td>${sw.name}</td><td><code>${sw.pattern}</code></td><td><small>${sw.url || ''}</small></td><td><button onclick="del(${i})">Delete</button></td></tr>`
      ).join('');
    }

    function del(i) { switches.splice(i, 1); render(); }

    function addSwitch() {
      const name = document.getElementById('new_name').value.trim();
      const pattern = document.getElementById('new_pat').value.trim();
      const url = document.getElementById('new_url').value.trim();
      if (!name || !pattern) { alert('Name and pattern required'); return; }
      switches.push({ name, pattern, url });
      render();
      document.getElementById('new_name').value = '';
      document.getElementById('new_pat').value = '';
      document.getElementById('new_url').value = '';
    }

    async function learnPattern() {
      const btn = document.getElementById('learn-btn');
      const st = document.getElementById('learn-status');
      btn.disabled = true;
      const deadline = Date.now() + 15000;
      st.style.color = '#888';
      st.textContent = 'Press your switch now...';
      while (Date.now() < deadline) {
        await new Promise(r => setTimeout(r, 500));
        try {
          const r = await fetch('/pattern');
          const d = await r.json();
          if (d.pattern) {
            document.getElementById('new_pat').value = d.pattern;
            st.style.color = 'green';
            st.textContent = 'Detected: ' + d.pattern;
            btn.disabled = false;
            return;
          }
        } catch(e) {}
      }
      st.style.color = 'red';
      st.textContent = 'Nothing detected. Try again.';
      btn.disabled = false;
    }

    function onSelChange() {
      const v = document.getElementById('ssid_sel').value;
      if (v) document.getElementById('ssid').value = v;
    }

    async function save() {
      const ssid = document.getElementById('ssid').value.trim();
      const password = document.getElementById('pass').value;
      const st = document.getElementById('status');
      st.textContent = 'Saving...';
      try {
        await fetch('/save', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ ssid, password, switches })
        });
        st.textContent = 'Saved! Device restarting...';
      } catch (e) {
        st.style.color = 'red';
        st.textContent = 'Error: ' + e;
      }
    }

    render();
  </script>
</body>
</html>
)=====";
