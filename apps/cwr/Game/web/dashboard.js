/*
 * Poseidon LAN 控制台的运行时脚本入口。
 *
 * 与 dashboard.css 一样，本文件在每次网页加载时从磁盘取得。可以把不需要
 * 游戏端 API 改动的交互、显示逻辑放在这里，保存并刷新浏览器即可测试。
 */
(function () {
    document.documentElement.dataset.poseidonDashboardAssets = "external";

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
