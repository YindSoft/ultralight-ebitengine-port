// app.js - loaded from embed.FS via VFS
function updateCounter(val) {
  document.getElementById('counter').textContent = val;
}

function showMessage(msg) {
  addLog('Go -> JS: ' + msg, 'from-go');
}

function addLog(text, cls) {
  var log = document.getElementById('log');
  var div = document.createElement('div');
  div.className = 'msg ' + (cls || '');
  div.textContent = '[' + new Date().toLocaleTimeString() + '] ' + text;
  log.appendChild(div);
  log.scrollTop = log.scrollHeight;
}

addLog('UI initialized from embed.FS', 'from-js');
