document.getElementById('controlActions').disabled = false;
document.getElementById('languageSelect').disabled = false;
for (const name of ['sendCommand', 'saveMeterSettings', 'toggleMeterScale', 'saveTuningSettings']) {
    const action = globalThis[name];
    globalThis[name] = (...args) => interactive(() => action(...args));
}
bindLanguageText();
setLanguage(language);
connectWS();
document.getElementById('advancedSettings').addEventListener('toggle', () => {
    if (document.getElementById('advancedSettings').open) loadAdvancedSettings();
});
startupPromise = loadInitialSettings();
startupPromise.then(() => {
    // Browser work only. The critical API sequence has already completed.
    if (language === 'ru') ensureLocale().catch(() => {});
    const start = () => {
        if (!document.hidden && !interactiveActions) loadVisuals();
        else setTimeout(start, 250);
    };
    if (typeof requestIdleCallback === 'function') requestIdleCallback(start, { timeout: 1500 });
    else setTimeout(start, 100);
});
