import base64, os

os.chdir('/home/xemowo/emMCP/example/XEMOWO_TEST20_MX')

labels = ['系统信息','温度','湿度','任务监控','UART 数据','AI 模块','猫咪动画','MCP 工具','红外学习']
subs   = ['项目名 / SHT3x 状态','实时温度数值','实时湿度数值','任务名 / 栈余量','AI 模组串口原始数据','AI 事件 / WiFi 状态','28 帧像素猫咪动画','已注册工具列表','红外学习 / 码组']

rows = ''
for i in range(0, 9, 3):
    row_imgs = ''
    row_labels = ''
    for j in range(3):
        idx = i + j
        with open(f'photo/屏幕{idx+1}.jpg', 'rb') as f:
            b64 = base64.b64encode(f.read()).decode()
        row_imgs += f'<td><img src="data:image/jpeg;base64,{b64}" width="240"></td>\n'
        row_labels += f'<td>屏幕 {idx+1}/9 — {labels[idx]}<br><small>{subs[idx]}</small></td>\n'
    rows += f'<tr>{row_labels}</tr>\n<tr>{row_imgs}</tr>\n<tr><td colspan="3" style="height:12px"></td></tr>\n'

html = '''<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>emMCP-Home OLED 屏幕实拍</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; }
  body {
    background: #0d1117; color: #c9d1d9;
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Helvetica, Arial, sans-serif;
    display: flex; flex-direction: column; align-items: center;
    padding: 32px 16px; min-height: 100vh;
  }
  h1 { font-size: 26px; margin-bottom: 6px; color: #f0f6fc; }
  .subtitle { color: #8b949e; margin-bottom: 28px; font-size: 14px; }
  table { border-collapse: collapse; max-width: 820px; width: 100%; }
  td {
    text-align: center; vertical-align: top;
    padding: 6px 10px; font-size: 14px; font-weight: 600;
    color: #f0f6fc;
  }
  td small { font-weight: 400; color: #8b949e; }
  td img { border-radius: 8px; border: 1px solid #30363d; }
  @media (max-width: 700px) {
    td img { width: 100% !important; height: auto; }
    h1 { font-size: 20px; }
  }
</style>
</head>
<body>

<h1>OLED 屏幕实拍</h1>
<p class="subtitle">九章验证板 &middot; 9 页交互界面 &middot; 双按键翻页</p>

<table>
''' + rows + '''
</table>

</body>
</html>'''

out = '/mnt/c/Users/lq950/Desktop/oled-screens.txt'
with open(out, 'w', encoding='utf-8') as f:
    f.write(html)

size_mb = os.path.getsize(out) / 1024 / 1024
print(f'Done! {size_mb:.1f} MB')
