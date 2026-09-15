(function () {
  const COOKIE = "strata-theme";
  const SUN =
    '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" aria-hidden="true"><circle cx="12" cy="12" r="4" /><path d="M12 3v2M12 19v2M5 12H3M21 12h-2M6.3 6.3l1.4 1.4M16.3 16.3l1.4 1.4M6.3 17.7l1.4-1.4M16.3 7.7l1.4-1.4" /></svg>';
  const MOON =
    '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" aria-hidden="true"><path d="M20 14.5A8.5 8.5 0 1 1 9.5 4 7 7 0 0 0 20 14.5z" /></svg>';

  function persist(theme) {
    try {
      localStorage.setItem(COOKIE, theme);
    } catch (_error) {
      /* private mode */
    }
    const secure = location.protocol === "https:" ? "; Secure" : "";
    const host = location.hostname;
    const domain = host === "getstrata.org" || host.endsWith(".getstrata.org") ? "; Domain=.getstrata.org" : "";
    document.cookie = `${COOKIE}=${theme}; Path=/; Max-Age=31536000; SameSite=Lax${domain}${secure}`;
  }

  function current() {
    return document.documentElement.classList.contains("light") ? "light" : "dark";
  }

  function apply(theme) {
    const next = theme === "light" ? "light" : "dark";
    document.documentElement.classList.remove("light", "dark");
    document.documentElement.classList.add(next);
    document.documentElement.style.colorScheme = next;
  }

  function syncButtons() {
    const toLight = current() === "dark";
    const label = toLight
      ? (window.STRATA_TRANSLATIONS?.[document.documentElement.lang]?.["nav.theme.light"] || "Passa al tema chiaro")
      : (window.STRATA_TRANSLATIONS?.[document.documentElement.lang]?.["nav.theme.dark"] || "Passa al tema scuro");
    document.querySelectorAll("[data-theme-toggle]").forEach((button) => {
      button.setAttribute("aria-label", label);
      button.innerHTML = toLight ? SUN : MOON;
    });
  }

  function toggle() {
    const next = current() === "dark" ? "light" : "dark";
    apply(next);
    persist(next);
    syncButtons();
  }

  document.addEventListener("DOMContentLoaded", () => {
    document.querySelectorAll("[data-theme-toggle]").forEach((button) => {
      button.addEventListener("click", toggle);
    });
    syncButtons();
  });

  document.addEventListener("strata:langchange", syncButtons);

  window.STRATA_THEME = { toggle, current, syncButtons };
})();
