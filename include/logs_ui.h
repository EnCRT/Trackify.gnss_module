#ifndef LOGS_UI_H
#define LOGS_UI_H

const char LOGS_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Trackify | Logs</title>
    <style>
        :root {
            --bg: #0f172a;
            --card: rgba(30, 41, 59, 0.7);
            --text: #f1f5f9;
            --muted: #94a3b8;
            --primary: #38bdf8;
            --accent: #818cf8;
            --border: rgba(255, 255, 255, 0.1);
            --shadow: 0 8px 32px 0 rgba(0, 0, 0, 0.37);
            --grad1: #0f172a;
            --grad2: #425068ff;
        }
        [data-theme="light"] {
            --bg: #f8fafc;
            --card: rgba(255, 255, 255, 0.8);
            --text: #1e293b;
            --muted: #64748b;
            --primary: #0ea5e9;
            --accent: #6366f1;
            --border: rgba(0, 0, 0, 0.05);
            --shadow: 0 8px 20px 0 rgba(157, 161, 221, 0.47);
            --grad1: #b8dcffff;
            --grad2: #f0f0f0ff;
        }
        body {
            background: linear-gradient(-45deg, var(--grad1), var(--grad2), var(--grad1));
            background-size: 400% 400%;
            animation: gradientBG 15s ease infinite;
            color: var(--text);
            font-family: 'Inter', system-ui, -apple-system, sans-serif;
            margin: 0;
            padding: 20px;
            min-height: 100vh;
            display: flex;
            justify-content: center;
            transition: all 0.4s ease;
        }
        @keyframes gradientBG {
            0% { background-position: 0% 50%; }
            50% { background-position: 100% 50%; }
            100% { background-position: 0% 50%; }
        }
        .container {
            width: 100%;
            max-width: 850px;
            animation: fadeIn 0.8s ease-out;
        }
        @keyframes fadeIn { from { opacity: 0; transform: translateY(10px); } to { opacity: 1; transform: translateY(0); } }
        .card {
            background: var(--card);
            backdrop-filter: blur(12px);
            -webkit-backdrop-filter: blur(12px);
            border: 1px solid var(--border);
            border-radius: 24px;
            padding: 32px;
            box-shadow: var(--shadow);
        }
        header {
            display: flex;
            flex-direction: column;
            gap: 24px;
            margin-bottom: 40px;
            border-bottom: 1px solid var(--border);
            padding-bottom: 30px;
        }
        .header-top { display: flex; justify-content: space-between; align-items: center; }
        .header-bottom { display: flex; justify-content: center; gap: 28px; }
        .brand { display: flex; align-items: center; gap: 12px; }
        .logo-emoji { 
            font-size: 2.3rem; 
            text-shadow: 0 0 10px rgba(0,0,0,0.1);
            transition: opacity 0.5s ease-in-out, transform 0.5s cubic-bezier(0.175, 0.885, 0.32, 1.275);
            display: inline-block;
        }
        .logo-emoji.fade { opacity: 0; transform: scale(0.8) rotate(-10deg); }
        h1 {
            margin: 0;
            font-size: 2.3rem;
            font-weight: 800;
            background: linear-gradient(135deg, var(--primary), var(--accent));
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
            letter-spacing: -0.025em;
        }
        .social-links a {
            color: var(--muted);
            transition: all 0.3s cubic-bezier(0.175, 0.885, 0.32, 1.275);
            display: flex;
            align-items: center;
        }
        .social-links a:hover { transform: scale(1.2) translateY(-2px); }
        .tiktok:hover { color: #000000ff !important; filter: drop-shadow(0 0 8px rgba(255, 0, 81, 0.14)); }
        .instagram:hover { color: #e1306c !important; filter: drop-shadow(0 0 8px rgba(240, 105, 150, 0.68)); }
        .telegram:hover { color: #0088cc !important; filter: drop-shadow(0 0 8px rgba(69, 185, 243, 0.56)); }
        .youtube:hover { color: #ff0000 !important; filter: drop-shadow(0 0 8px rgba(255, 0, 0, 0.43)); }
        .social-links svg { width: 26px; height: 26px; fill: currentColor; }
        .theme-toggle {
            background: var(--bg);
            border: 1px solid var(--border);
            color: var(--text);
            border-radius: 12px;
            padding: 8px 16px;
            cursor: pointer;
            font-weight: 600;
            font-size: 0.875rem;
            transition: all 0.3s;
            display: flex;
            align-items: center;
            gap: 8px;
        }
        .theme-toggle:hover { border-color: var(--primary); transform: translateY(-1px); }
        .table-container { overflow-x: auto; }
        table { width: 100%; border-collapse: separate; border-spacing: 0 12px; margin-top: -12px; }
        th { text-align: left; padding: 12px 16px; color: var(--muted); font-size: 0.75rem; text-transform: uppercase; letter-spacing: 0.05em; font-weight: 700; }
        tr { transition: all 0.3s ease; }
        td { background: rgba(56, 189, 248, 0.03); padding: 12px 16px; border-top: 1px solid var(--border); border-bottom: 1px solid var(--border); transition: all 0.3s ease; }
        tr:hover td { background: rgba(56, 189, 248, 0.08); border-color: var(--primary); }
        td:first-child { border-left: 1px solid var(--border); border-radius: 12px 0 0 12px; width: 40px; text-align: center; color: var(--muted); }
        td:last-child { border-right: 1px solid var(--border); border-radius: 0 12px 12px 0; text-align: right; }
        .file-name { font-weight: 600; color: var(--text); }
        .file-size { font-size: 0.85rem; color: var(--muted); font-weight: 500; }
        .btn {
            background: linear-gradient(135deg, var(--primary), var(--accent));
            color: white !important;
            text-decoration: none;
            padding: 8px 16px;
            border-radius: 10px;
            font-size: 0.8125rem;
            font-weight: 700;
            transition: all 0.3s;
            display: inline-flex;
            align-items: center;
            box-shadow: 0 4px 12px rgba(56, 189, 248, 0.2);
            outline: none;
            border: 0;
            position: relative;
        }
        .btn.loading { color: transparent !important; pointer-events: none; }
        .btn.loading::after {
            content: "";
            position: absolute;
            width: 16px;
            height: 16px;
            top: 50%;
            left: 50%;
            margin-top: -8px;
            margin-left: -8px;
            border: 2px solid rgba(255,255,255,0.3);
            border-radius: 50%;
            border-top-color: #fff;
            animation: spin 0.6s linear infinite;
        }
        @keyframes spin { to { transform: rotate(360deg); } }
        .btn:hover { transform: translateY(-1px); box-shadow: 0 6px 16px rgba(56, 189, 248, 0.3); filter: brightness(1.1); }
        .btn-danger { background: linear-gradient(135deg, #ef4444, #dc2626); box-shadow: 0 4px 12px rgba(239, 68, 68, 0.2); border: none; cursor: pointer; }
        .btn-danger:hover { box-shadow: 0 6px 16px rgba(239, 68, 68, 0.3); }
        .action-buttons { display: flex; gap: 8px; justify-content: flex-end; }
        @media (max-width: 600px) {
            .container { padding: 10px; }
            .card { padding: 16px; border-radius: 16px; }
            h1 { font-size: 1.6rem; }
            .header-top { flex-direction: column; gap: 12px; text-align: center; }
            .header-bottom { gap: 16px; }

            /* Table to Card Transformation */
            .table-container { overflow: visible; }
            table, thead, tbody, th, td, tr { display: block; }
            thead { display: none; }
            
            table { margin-top: 0; }
            tr { 
                background: rgba(56, 189, 248, 0.05);
                border: 1px solid var(--border);
                border-radius: 16px;
                padding: 16px;
                margin-bottom: 16px;
                position: relative;
                transition: transform 0.2s ease;
            }
            tr:hover { transform: translateY(-2px); border-color: var(--primary); }
            
            td { 
                padding: 4px 0; 
                background: none !important; 
                border: none !important;
                text-align: left !important;
                width: auto !important;
            }
            
            td:first-child { 
                font-size: 0.7rem;
                color: var(--muted);
                margin-bottom: 4px;
            }
            
            .file-name { 
                font-size: 1.1rem; 
                display: block;
                margin-bottom: 4px;
                word-break: break-all;
            }
            
            .file-size {
                display: block;
                margin-bottom: 12px;
            }

            .action-buttons { 
                display: flex; 
                flex-wrap: wrap; 
                gap: 8px; 
                justify-content: flex-start;
                border-top: 1px solid var(--border);
                padding-top: 12px;
                margin-top: 8px;
            }
            
            .btn {
                flex: 1 1 calc(50% - 8px);
                justify-content: center;
                padding: 10px;
                font-size: 0.8rem;
            }
            
            .btn-danger {
                flex: 1 1 100%;
            }
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="card">
            <header>
                <div class="header-top">
                    <div class="brand">
                        <span class="logo-emoji" id="logo-emoji">🏁</span>
                        <h1>Trackify</h1>
                    </div>
                    <button class="theme-toggle" onclick="toggleTheme()" id="theme-btn">
                        <span id="theme-icon">🌙</span>
                        <span id="theme-text">Dark Mode</span>
                    </button>
                </div>
                <div class="header-bottom social-links">
                    <a href="https://www.tiktok.com/" target="_blank" title="TikTok" class="tiktok">
                        <svg viewBox="0 0 448 512"><path d="M448,209.91a210.06,210.06,0,0,1-122.77-39.25V349.38A162.55,162.55,0,1,1,185,188.31V278.2a74.62,74.62,0,1,0,52.23,71.18V0l88,0a121.18,121.18,0,0,0,1.86,22.17h0A122.18,122.18,0,0,0,381,102.39a121.43,121.43,0,0,0,67,20.14Z"/></svg>
                    </a>
                    <a href="https://www.instagram.com/" target="_blank" title="Instagram" class="instagram">
                        <svg viewBox="0 0 24 24"><path d="M12 2.163c3.204 0 3.584.012 4.85.07 3.252.148 4.771 1.691 4.919 4.919.058 1.265.069 1.645.069 4.849 0 3.205-.012 3.584-.069 4.849-.149 3.225-1.664 4.771-4.919 4.919-1.266.058-1.644.07-4.85.07-3.204 0-3.584-.012-4.849-.07-3.26-.149-4.771-1.699-4.919-4.92-.058-1.265-.07-1.644-.07-4.849 0-3.204.013-3.583.07-4.849.149-3.227 1.664-4.771 4.919-4.919 1.266-.057 1.645-.069 4.849-.069zm0-2.163c-3.259 0-3.667.014-4.947.072-4.358.2-6.78 2.618-6.98 6.98-.059 1.281-.073 1.689-.073 4.948 0 3.259.014 3.668.072 4.948 0.2 4.358 2.618 6.78 6.98 6.98 1.281.058 1.689.072 4.948.072 3.259 0 3.668-.014 4.948-.072 4.354-.2 6.782-2.618 6.979-6.98.059-1.28.073-1.689.073-4.948 0-3.259-.014-3.667-.072-4.947-0.196-4.354-2.617-6.78-6.979-6.98-1.281-.059-1.69-.073-4.949-.073zm0 5.838c-3.403 0-6.162 2.759-6.162 6.162s2.759 6.163 6.162 6.163 6.162-2.759 6.162-6.163c0-3.403-2.759-6.162-6.162-6.162zm0 10.162c-2.209 0-4-1.79-4-4 0-2.209 1.791-4 4-4s4 1.791 4 4c0 2.21-1.791 4-4 4zm6.406-11.845c0.796 0 1.441.645 1.441 1.44s-0.645 1.44-1.441 1.44c-0.795 0-1.44-0.645-1.44-1.44s0.645-1.44 1.44-1.44z"/></svg>
                    </a>
                    <a href="https://t.me/" target="_blank" title="Telegram" class="telegram">
                        <svg viewBox="0 0 496 512"><path d="M248,8C111.033,8,0,119.033,0,256S111.033,504,248,504,496,392.967,496,256,384.967,8,248,8ZM362.952,176.66c-3.732,39.215-19.881,134.378-28.1,178.3-3.476,18.584-10.322,24.816-16.948,25.425-14.4,1.326-25.338-9.517-39.287-18.661-21.827-14.308-34.158-23.215-55.346-37.177-24.485-16.135-8.612-25,5.342-39.5,3.652-3.793,67.107-61.51,68.335-66.746.153-.655.3-3.1-1.15-4.391s-3.59-.849-5.135-.5c-2.192.495-37.03,23.516-104.329,69.062-9.864,6.769-18.8,10.14-26.81,9.967-8.823-.19-25.791-4.982-38.411-9.074-15.479-5.018-27.792-7.674-26.721-16.195.558-4.437,6.666-8.954,18.328-13.552,71.93-31.332,119.865-51.96,143.8-61.884,68.31-28.312,82.48-33.227,91.751-33.391,2.04-.036,6.582.466,9.547,2.872a11.1,11.1,0,0,1,3.31,5.811C363.308,169.194,363.166,172.96,362.952,176.66Z"/></svg>
                    </a>
                    <a href="https://www.youtube.com/" target="_blank" title="YouTube" class="youtube">
                        <svg viewBox="0 0 24 24"><path d="M23.498 6.186a3.016 3.016 0 0 0-2.122-2.136C19.505 3.545 12 3.545 12 3.545s-7.505 0-9.377.505A3.017 3.017 0 0 0 .502 6.186C0 8.07 0 12 0 12s0 3.93.502 5.814a3.016 3.016 0 0 0 2.122 2.136c1.871.505 9.376.505 9.376.505s7.505 0 9.377-.505a3.015 3.015 0 0 0 2.122-2.136C24 15.93 24 12 24 12s0-3.93-.502-5.814zM9.545 15.568V8.432L15.818 12l-6.273 3.568z"/></svg>
                    </a>
                </div>
            </header>
            <div class="table-container">
                <table>
                    <thead>
                        <tr>
                            <th>#</th>
                            <th>Name</th>
                            <th>Size</th>
                            <th></th>
                        </tr>
                    </thead>
                    <tbody>
                        %FILE_LIST%
                    </tbody>
                </table>
            </div>
        </div>
    </div>
    <script>
        function toggleTheme() {
            const body = document.documentElement;
            const currentTheme = body.getAttribute('data-theme');
            const newTheme = currentTheme === 'light' ? 'dark' : 'light';
            body.setAttribute('data-theme', newTheme);
            localStorage.setItem('theme', newTheme);
            updateUI(newTheme);
        }
        function updateUI(theme) {
            const btnText = document.getElementById('theme-text');
            const btnIcon = document.getElementById('theme-icon');
            if (theme === 'light') {
                btnText.innerText = 'Dark Mode';
                btnIcon.innerText = '🌙';
            } else {
                btnText.innerText = 'Light Mode';
                btnIcon.innerText = '☀️';
            }
        }
        const savedTheme = localStorage.getItem('theme') || 'dark';
        document.documentElement.setAttribute('data-theme', savedTheme);
        updateUI(savedTheme);

        async function downloadTxt(btn, fileName) {
            btn.classList.add('loading');
            try {
                const response = await fetch(`/download?file=${encodeURIComponent(fileName)}`);
                if (!response.ok) throw new Error('Download failed');
                const blob = await response.blob();
                const url = window.URL.createObjectURL(blob);
                const a = document.createElement('a');
                a.href = url;
                a.download = fileName;
                document.body.appendChild(a);
                a.click();
                window.URL.revokeObjectURL(url);
            } catch (err) {
                alert('Ошибка при скачивании файла');
            } finally {
                btn.classList.remove('loading');
            }
        }

        async function downloadGPX(btn, fileName) {
            btn.classList.add('loading');
            try {
                const response = await fetch(`/download?file=${encodeURIComponent(fileName)}`);
                if (!response.ok) throw new Error('Network response was not ok');
                
                const reader = response.body.getReader();
                const decoder = new TextDecoder();
                let { value, done } = await reader.read();
                let text = decoder.decode(value);
                
                // If the file is large, we might need to read it in chunks, 
                // but for now, let's assume it fits in memory or read all
                while (!done) {
                    ({ value, done } = await reader.read());
                    if (value) text += decoder.decode(value);
                }

                const gpxContent = convertNmeaToGpx(text, fileName);
                const blob = new Blob([gpxContent], { type: 'application/gpx+xml' });
                const url = window.URL.createObjectURL(blob);
                const a = document.createElement('a');
                a.style.display = 'none';
                a.href = url;
                a.download = fileName.replace('.txt', '.gpx');
                document.body.appendChild(a);
                a.click();
                window.URL.revokeObjectURL(url);
            } catch (err) {
                console.error('GPX Conversion Error:', err);
                alert('Ошибка при конвертации в GPX');
            } finally {
                btn.classList.remove('loading');
            }
        }

        function convertNmeaToGpx(nmeaText, fileName) {
            // Some logs might not have \n, they might just have $ to start a new sentence.
            // Let's ensure we split correctly.
            const rawSentences = nmeaText.split('$');
            let gpx = `<?xml version="1.0" encoding="UTF-8"?>\n`;
            gpx += `<gpx version="1.1" creator="Trackify" xmlns="http://www.topografix.com/GPX/1/1">\n`;
            gpx += `  <trk>\n    <name>${fileName}</name>\n    <trkseg>\n`;

            let currentPoint = null;
            let lastValidDate = null;

            for (let raw of rawSentences) {
                if (raw.trim().length === 0) continue;
                
                // Remove trailing checksums/newlines for cleaner parsing
                const cleanLine = raw.split('*')[0].trim();
                const parts = cleanLine.split(',');
                if (parts.length < 1) continue;
                
                const type = parts[0].substring(2); // GPRMC -> RMC (or GNRMC -> RMC)

                if (type === 'RMC') {
                    if (parts[2] !== 'A' && parts[2] !== 'V') continue; // V is warning, but sometimes has valid coords
                    
                    const time = parts[1]; 
                    const latRaw = parts[3];
                    const latDir = parts[4];
                    const lonRaw = parts[5];
                    const lonDir = parts[6];
                    const speedKnots = parts[7];
                    const courseDeg = parts[8];
                    const date = parts[9];

                    if (latRaw && lonRaw) {
                        try {
                            const lat = parseNmeaCoord(latRaw, latDir);
                            const lon = parseNmeaCoord(lonRaw, lonDir);
                            let timestamp = null;
                            if (date && time) {
                                timestamp = parseNmeaDateTime(date, time);
                                lastValidDate = date; // Cache date
                            } else if (time && lastValidDate) {
                                timestamp = parseNmeaDateTime(lastValidDate, time);
                            }

                            const speedKmph = speedKnots ? (parseFloat(speedKnots) * 1.852) : null;
                            const speedFinal = isNaN(speedKmph) ? null : speedKmph;
                            const courseFinal = courseDeg ? parseFloat(courseDeg) : null;
                            const courseValid = isNaN(courseFinal) ? null : courseFinal;
                            
                            if (currentPoint && currentPoint.lat !== lat && currentPoint.lon !== lon) {
                                gpx += formatPoint(currentPoint);
                            } else if (currentPoint && currentPoint.time !== timestamp) {
                                // Same coords but different time (e.g. stopped)
                                gpx += formatPoint(currentPoint);
                            }
                            
                            currentPoint = { lat, lon, time: timestamp, ele: null, speed: speedFinal, course: courseValid };
                        } catch(e) { /* ignore parse errors for single lines */ }
                    }
                } else if (type === 'GGA' && currentPoint) {
                    const time = parts[1];
                    // Sync by integer seconds
                    if (currentPoint.time && time && currentPoint.time.includes(`T${time.substring(0, 2)}:${time.substring(2, 4)}:${time.substring(4, 6)}`)) {
                        const alt = parts[9];
                        if (alt) currentPoint.ele = parseFloat(alt).toFixed(1);
                    }
                }
            }
            
            if (currentPoint) {
                gpx += formatPoint(currentPoint);
            }

            gpx += `    </trkseg>\n  </trk>\n</gpx>`;
            return gpx;
        }
        function formatPoint(p) {
            let pt = `      <trkpt lat="${p.lat.toFixed(7)}" lon="${p.lon.toFixed(7)}">\n`;
            if (p.ele) pt += `        <ele>${p.ele}</ele>\n`;
            if (p.time) pt += `        <time>${p.time}</time>\n`;
            if (p.speed !== null || p.course !== null) {
                pt += `        <extensions>\n`;
                if (p.speed !== null) pt += `          <speed>${(p.speed / 3.6).toFixed(2)}</speed>\n`; // GPX speed is m/s
                if (p.course !== null) pt += `          <course>${p.course.toFixed(1)}</course>\n`;
                pt += `        </extensions>\n`;
            }
            pt += `      </trkpt>\n`;
            return pt;
        }

        function parseNmeaCoord(raw, dir) {
            const dotIdx = raw.indexOf('.');
            const degs = parseInt(raw.substring(0, dotIdx - 2));
            const mins = parseFloat(raw.substring(dotIdx - 2));
            let dec = degs + (mins / 60);
            if (dir === 'S' || dir === 'W') dec = -dec;
            return dec;
        }

        function parseNmeaDateTime(date, time) {
            if (!date || !time) return null;
            // date: DDMMYY, time: HHMMSS.SS...
            const dMatch = date.match(/^(\d{2})(\d{2})(\d{2})/);
            const tMatch = time.match(/^(\d{2})(\d{2})(\d{2})/);
            if (!dMatch || !tMatch) return null;
            
            const day = dMatch[1];
            const month = dMatch[2];
            const year = '20' + dMatch[3];
            const hour = tMatch[1];
            const min = tMatch[2];
            const sec = tMatch[3];
            
            return `${year}-${month}-${day}T${hour}:${min}:${sec}Z`;
        }

        function deleteFile(fileName) {
            if (confirm(`Вы уверены, что хотите удалить файл ${fileName}?`)) {
                fetch(`/delete?file=${encodeURIComponent(fileName)}`, { method: 'POST' })
                    .then(response => {
                        if (response.ok) {
                            window.location.reload();
                        } else {
                            alert('Ошибка при удалении файла');
                        }
                    })
                    .catch(err => alert('Ошибка сети'));
            }
        }

        // Dynamic Emoji Logic
        const emojis = ['🏍️', '🏎️', '🏁', '⏱️', '🥇', '🏆', '💨', '⚡', '🚥'];
        let currentIndex = 0;
        const emojiEl = document.getElementById('logo-emoji');

        setInterval(() => {
            emojiEl.classList.add('fade');
            setTimeout(() => {
                currentIndex = (currentIndex + 1) % emojis.length;
                emojiEl.innerText = emojis[currentIndex];
                emojiEl.classList.remove('fade');
            }, 500);
        }, 2000);
    </script>
</body>
</html>
)rawliteral";

#endif
