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
