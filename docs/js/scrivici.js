(function () {
  const FEEDBACK_URL = "https://strata-be-372580174147.europe-west1.run.app/v1/feedback";
  const EMAIL_PATTERN = /^[^@\s]+@[^@\s]+\.[^@\s]+$/;

  function t(key) {
    const lang = document.documentElement.lang || "it";
    const dicts = window.STRATA_TRANSLATIONS || {};
    return dicts[lang]?.[key] || dicts.it?.[key] || "";
  }

  function isValid(email, title, description) {
    return EMAIL_PATTERN.test(email.trim())
      && title.trim().length >= 3
      && title.trim().length <= 120
      && description.trim().length >= 10
      && description.trim().length <= 4000;
  }

  function ensureDialog() {
    let root = document.getElementById("scrivici-root");
    if (root) return root;

    root = document.createElement("div");
    root.id = "scrivici-root";
    root.hidden = true;
    root.innerHTML = `
      <div class="scrivici-overlay" data-scrivici-close></div>
      <div class="scrivici-frame">
        <div class="scrivici-dialog" role="dialog" aria-modal="true" aria-labelledby="scrivici-heading">
          <h2 id="scrivici-heading" data-i18n="nav.scrivici">Scrivici</h2>
          <p class="scrivici-lead" data-i18n="scrivici.lead"></p>
          <p class="scrivici-success" data-scrivici-success hidden data-i18n="scrivici.success"></p>
          <form class="scrivici-form" novalidate>
            <label>
              <span data-i18n="scrivici.email">Email</span>
              <input type="email" name="email" required autocomplete="email">
            </label>
            <label>
              <span data-i18n="scrivici.title">Titolo</span>
              <input type="text" name="title" required minlength="3" maxlength="120">
            </label>
            <label>
              <span data-i18n="scrivici.message">Messaggio</span>
              <textarea name="description" required minlength="10" maxlength="4000" rows="5"></textarea>
            </label>
            <div class="scrivici-honeypot" aria-hidden="true">
              <label>
                <span data-i18n="scrivici.website">Sito web</span>
                <input type="text" name="website" tabindex="-1" autocomplete="off">
              </label>
            </div>
            <p class="scrivici-error" data-scrivici-error hidden></p>
            <div class="scrivici-actions">
              <button type="button" class="scrivici-cancel" data-scrivici-close data-i18n="scrivici.cancel">Annulla</button>
              <button type="submit" class="scrivici-submit" data-i18n="scrivici.send">Invia</button>
            </div>
          </form>
          <button type="button" class="scrivici-done" data-scrivici-close hidden data-i18n="scrivici.close">Chiudi</button>
        </div>
      </div>
    `;
    document.body.appendChild(root);
    bindDialog(root);
    applyDialogCopy(root);
    return root;
  }

  function applyDialogCopy(root) {
    root.querySelectorAll("[data-i18n]").forEach((el) => {
      const key = el.getAttribute("data-i18n");
      const value = t(key);
      if (value) el.textContent = value;
    });
  }

  function setOpen(root, open) {
    root.hidden = !open;
    document.body.classList.toggle("scrivici-locked", open);
    if (!open) return;
    applyDialogCopy(root);
    const email = root.querySelector('input[name="email"]');
    email?.focus();
  }

  function resetForm(root) {
    const form = root.querySelector("form");
    const success = root.querySelector("[data-scrivici-success]");
    const done = root.querySelector(".scrivici-done");
    const error = root.querySelector("[data-scrivici-error]");
    form?.reset();
    if (form) form.hidden = false;
    if (success) success.hidden = true;
    if (done) done.hidden = true;
    if (error) {
      error.hidden = true;
      error.textContent = "";
    }
    syncSubmit(root);
  }

  function syncSubmit(root) {
    const form = root.querySelector("form");
    const submit = root.querySelector(".scrivici-submit");
    if (!form || !submit) return;
    const data = new FormData(form);
    submit.disabled = !isValid(
      String(data.get("email") || ""),
      String(data.get("title") || ""),
      String(data.get("description") || ""),
    ) || submit.dataset.sending === "true";
  }

  function bindDialog(root) {
    root.querySelectorAll("[data-scrivici-close]").forEach((el) => {
      el.addEventListener("click", () => {
        setOpen(root, false);
        resetForm(root);
      });
    });

    const form = root.querySelector("form");
    form?.addEventListener("input", () => syncSubmit(root));
    form?.addEventListener("submit", async (event) => {
      event.preventDefault();
      const submit = root.querySelector(".scrivici-submit");
      const error = root.querySelector("[data-scrivici-error]");
      const data = new FormData(form);
      const email = String(data.get("email") || "").trim();
      const title = String(data.get("title") || "").trim();
      const description = String(data.get("description") || "").trim();
      const website = String(data.get("website") || "");
      if (!isValid(email, title, description) || submit?.dataset.sending === "true") return;

      if (submit) {
        submit.dataset.sending = "true";
        submit.disabled = true;
        submit.textContent = t("scrivici.sending");
      }
      if (error) {
        error.hidden = true;
        error.textContent = "";
      }

      try {
        const response = await fetch(FEEDBACK_URL, {
          method: "POST",
          headers: { "content-type": "application/json" },
          body: JSON.stringify({ email, title, description, website }),
        });
        if (!response.ok && response.status !== 204) {
          throw new Error(String(response.status));
        }
        form.hidden = true;
        const success = root.querySelector("[data-scrivici-success]");
        const done = root.querySelector(".scrivici-done");
        if (success) success.hidden = false;
        if (done) done.hidden = false;
      } catch (_caught) {
        if (error) {
          error.textContent = t("scrivici.error");
          error.hidden = false;
        }
      } finally {
        if (submit) {
          submit.dataset.sending = "false";
          submit.textContent = t("scrivici.send");
          syncSubmit(root);
        }
      }
    });
  }

  function init() {
    const root = ensureDialog();
    document.querySelectorAll("[data-open-scrivici]").forEach((button) => {
      button.addEventListener("click", () => {
        resetForm(root);
        setOpen(root, true);
      });
    });
    document.addEventListener("keydown", (event) => {
      if (event.key === "Escape" && !root.hidden) {
        setOpen(root, false);
        resetForm(root);
      }
    });
    document.addEventListener("strata:langchange", () => applyDialogCopy(root));
    syncSubmit(root);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
