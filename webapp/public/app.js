const widthRange = document.getElementById('widthRange');
const widthInput = document.getElementById('widthInput');
const snippetInput = document.getElementById('snippetInput');
const renderButton = document.getElementById('renderButton');
const statusText = document.getElementById('statusText');
const metricsText = document.getElementById('metricsText');
const pdfFrame = document.getElementById('pdfFrame');

function setWidth(value) {
  const width = Math.max(180, Math.min(430, Number(value) || 360));
  widthRange.value = width;
  widthInput.value = width;
}

widthRange.addEventListener('input', () => setWidth(widthRange.value));
widthInput.addEventListener('change', () => setWidth(widthInput.value));

async function render() {
  const snippet = snippetInput.value.trim();
  if (!snippet) {
    statusText.textContent = '请输入 snippet';
    return;
  }
  renderButton.disabled = true;
  statusText.textContent = '排版中';
  metricsText.textContent = '';
  try {
    const response = await fetch('/api/render', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        snippet,
        widthPt: Number(widthInput.value),
      }),
    });
    const data = await response.json();
    if (!response.ok) {
      throw new Error(data.error || 'Render failed');
    }
    pdfFrame.src = `${data.pdfUrl}?t=${Date.now()}`;
    const item = data.summary.results[0];
    statusText.textContent = '完成';
    metricsText.textContent = `${item.requestToPdfMs} ms / ${item.pdfBytes} bytes`;
  } catch (error) {
    statusText.textContent = '排版失败';
    metricsText.textContent = error.message;
  } finally {
    renderButton.disabled = false;
  }
}

renderButton.addEventListener('click', render);
snippetInput.addEventListener('keydown', (event) => {
  if ((event.ctrlKey || event.metaKey) && event.key === 'Enter') {
    render();
  }
});
