/*
 * Poseidon LAN 控制台的运行时脚本入口。
 *
 * 与 dashboard.css 一样，本文件在每次网页加载时从磁盘取得。可以把不需要
 * 游戏端 API 改动的交互、显示逻辑放在这里，保存并刷新浏览器即可测试。
 */
(function () {
    document.documentElement.dataset.poseidonDashboardAssets = "external";

    // Use the browser/OS preference for the initial map palette.  This runs
    // after the embedded dashboard has created its map controls.
    const systemTheme = window.matchMedia("(prefers-color-scheme: dark)");
    const syncMapTheme = (dark) => {
        if (typeof darkMapControl === "undefined")
            return;
        darkMapControl.checked = dark;
        darkMap = dark;
        vectorTiles.clear();
        draw();
    };
    syncMapTheme(systemTheme.matches);
    systemTheme.addEventListener("change", (event) => syncMapTheme(event.matches));

    const oldStatus = document.querySelector("header .warn");
    if (oldStatus) {
        const fullscreenButton = document.createElement("button");
        fullscreenButton.type = "button";
        fullscreenButton.className = "fullscreen-toggle";
        oldStatus.replaceWith(fullscreenButton);

        const updateFullscreenLabel = () => {
            fullscreenButton.textContent = document.fullscreenElement ? "退出全屏" : "进入全屏";
        };
        fullscreenButton.addEventListener("click", async () => {
            try {
                if (document.fullscreenElement)
                    await document.exitFullscreen();
                else
                    await document.documentElement.requestFullscreen();
            } catch (_) {
                // Browsers may deny fullscreen outside a user gesture.
                updateFullscreenLabel();
            }
        });
        document.addEventListener("fullscreenchange", updateFullscreenLabel);
        updateFullscreenLabel();
    }

    const lockCard = document.querySelector(".lock-card");
    if (lockCard) {
        lockCard.classList.add("info-card");
        lockCard.innerHTML = '<strong>信息</strong><pre id="info"></pre><pre id="lock" hidden></pre>';
    }

    const info = document.querySelector("#info");
    let selectedUnitId = null;
    let selectedPoint = null;
    const selectedUnit = () => (state.units || []).find((unit) =>
        unit.status === "active" && String(unit.id) === String(selectedUnitId));
    const distanceToPlayer = (unit) => {
        const player = (state.units || []).find((entry) => entry.isPlayer && entry.status === "active");
        return player ? Math.hypot(unit.x - player.x, unit.y - player.y, unit.z - player.z) : null;
    };
    const updateInfo = () => {
        if (!info)
            return;
        const unit = selectedUnit();
        if (unit) {
            const distance = distanceToPlayer(unit);
            info.textContent = [
                `类型: ${unit.display || unit.type}`,
                `阵营: ${unit.side} (${unit.relation})`,
                `距离: ${distance === null ? "—" : `${Math.round(distance)} m`}`,
                `Damage: ${Number(unit.damage || 0).toFixed(3)}`,
                `地图格: ${unit.gridRef || "—"}`,
                `坐标: X ${Number(unit.x).toFixed(1)}, Y ${Number(unit.y).toFixed(1)}, Z ${Number(unit.z).toFixed(1)}`,
                `速度: ${Math.round(Number(unit.speed || 0) * 10) / 10} m/s`,
            ].join("\n");
        } else if (selectedPoint) {
            info.textContent = [
                "地图坐标",
                `X ${selectedPoint.x.toFixed(1)}, Z ${selectedPoint.z.toFixed(1)}`,
            ].join("\n");
        } else {
            info.textContent = "";
        }
    };
    const selectUnit = (id) => {
        selectedUnitId = id;
        selectedPoint = null;
        updateInfo();
        draw();
        void lock(id);
    };
    const markerBeforeSelection = marker;
    marker = (unit, x, y) => {
        markerBeforeSelection(unit, x, y);
        if (String(unit.id) !== String(selectedUnitId || ""))
            return;
        const distance = distanceToPlayer(unit);
        const label = `${unit.display || unit.type}${distance === null ? "" : ` · ${Math.round(distance)} m`}`;
        ctx.save();
        ctx.strokeStyle = "#168dff";
        ctx.lineWidth = 4;
        ctx.strokeRect(x - 33, y - 33, 66, 66);
        ctx.font = "600 13px system-ui,sans-serif";
        ctx.textBaseline = "middle";
        const labelWidth = ctx.measureText(label).width;
        ctx.fillStyle = "rgba(3, 22, 42, .86)";
        ctx.fillRect(x + 38, y - 12, labelWidth + 12, 24);
        ctx.fillStyle = "#bde8ff";
        ctx.fillText(label, x + 44, y);
        ctx.restore();
    };
    const pickBeforeSelection = pick;
    pick = (x, y) => {
        let closest = null;
        let distanceSquared = 34 * 34;
        for (const point of pts) {
            const candidateDistance = (point[0] - x) ** 2 + (point[1] - y) ** 2;
            if (candidateDistance < distanceSquared) {
                distanceSquared = candidateDistance;
                closest = point[2];
            }
        }
        if (closest) {
            selectUnit(closest.id);
            return;
        }
        selectedUnitId = null;
        selectedPoint = world(x, y);
        updateInfo();
        draw();
    };
    const rowsBeforeSelection = rows;
    rows = () => {
        rowsBeforeSelection();
        document.querySelectorAll("#units tr[data-id]").forEach((row) => {
            row.onclick = () => selectUnit(row.dataset.id);
        });
    };
    setInterval(() => {
        if (selectedUnitId && !selectedUnit())
            selectedUnitId = null;
        updateInfo();
    }, 500);

    // Small public hook for local frontend experiments.  It deliberately does
    // not retain game state: a normal browser refresh remains the reliable way
    // to pick up a saved CSS or JavaScript change.
    window.PoseidonDashboard = {
        refreshStyles() {
            for (const sheet of document.querySelectorAll('link[href="/web/dashboard.css"]')) {
                sheet.href = `/web/dashboard.css?reload=${Date.now()}`;
            }
        },
    };
}());
