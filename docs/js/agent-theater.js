(function () {
  const SCRIPTS = {
    map: {
      period: 1700,
      beats: [
        { hit: "layer", click: true, on: ["layer"] },
        { hit: "identify", click: true, on: ["layer", "identify"] },
        { hit: "parcel", click: true, on: ["layer", "identify", "parcel", "popup"] },
        { hit: "buffer", click: true, on: ["layer", "identify", "parcel", "popup", "buffer", "ring"] },
      ],
    },
    routine: {
      period: 1900,
      beats: [
        { hit: "prompt", on: ["prompt"] },
        { hit: "run", click: true, on: ["prompt", "run", "working"] },
        { hit: "status", on: ["prompt", "run", "working"] },
        { hit: "file", click: true, on: ["prompt", "run", "done", "file"] },
      ],
    },
    context: {
      period: 1800,
      beats: [
        { hit: "ask", click: true, on: ["ask"] },
        { hit: "rule", click: true, on: ["ask", "rule"] },
        { hit: "reply", on: ["ask", "rule", "reply"] },
        { hit: "skill", click: true, on: ["ask", "rule", "reply", "skill"] },
      ],
    },
    team: {
      period: 1600,
      beats: [
        { hit: "rilievo", click: true, on: ["rilievo"] },
        { hit: "export", click: true, on: ["rilievo", "export"] },
        { hit: "gallery", click: true, on: ["rilievo", "export", "gallery"] },
        { hit: "sync", on: ["rilievo", "export", "gallery", "sync"] },
      ],
    },
  };

  function t(key) {
    const lang = document.documentElement.lang || "it";
    const dicts = window.STRATA_TRANSLATIONS || {};
    return dicts[lang]?.[key] || dicts.it?.[key] || "";
  }

  function reducedMotion() {
    return window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  }

  function setOn(stage, keys) {
    const active = new Set(keys);
    stage.querySelectorAll("[data-hit], [data-on]").forEach((node) => {
      const token = node.getAttribute("data-hit") || node.getAttribute("data-on");
      node.classList.toggle("is-on", active.has(token));
    });
    const status = stage.querySelector("[data-status-idle]");
    if (status) {
      if (active.has("done")) status.textContent = t("theater.done") || "Fatto";
      else if (active.has("working")) status.textContent = t("theater.working") || "In corso…";
      else status.textContent = t("theater.wait") || "In attesa";
    }
    stage.querySelectorAll(".mini-agent").forEach((agent) => {
      const state = agent.querySelector("[data-agent-state]");
      if (!state) return;
      state.textContent = agent.classList.contains("is-on")
        ? (t("theater.running") || "in corso")
        : (t("theater.launch") || "Avvia");
    });
  }

  function moveCursor(stage, hit) {
    const cursor = stage.querySelector(".feature-cursor");
    const target = stage.querySelector(`[data-hit="${hit}"]`);
    if (!cursor || !target) return;
    const stageBox = stage.getBoundingClientRect();
    const hitBox = target.getBoundingClientRect();
    if (hitBox.width < 2 && hitBox.height < 2) return;
    cursor.style.left = `${hitBox.left - stageBox.left + hitBox.width * 0.55}px`;
    cursor.style.top = `${hitBox.top - stageBox.top + hitBox.height * 0.5}px`;
  }

  function playStage(stage, config, reduce) {
    const cursor = stage.querySelector(".feature-cursor");
    if (reduce) {
      if (cursor) cursor.remove();
      setOn(stage, config.beats.flatMap((beat) => beat.on || []));
      return;
    }

    let beat = 0;
    let clickTimer;
    let pressTimer;

    function applyBeat() {
      const step = config.beats[beat];
      const prior = [];
      for (let index = 0; index < beat; index += 1) {
        (config.beats[index].on || []).forEach((key) => prior.push(key));
      }
      setOn(stage, prior);
      stage.querySelectorAll("[data-hit]").forEach((node) => {
        node.classList.toggle("is-hot", node.getAttribute("data-hit") === step.hit);
        node.classList.remove("is-press");
      });
      moveCursor(stage, step.hit);
      if (cursor) cursor.classList.remove("is-clicking");
      window.clearTimeout(clickTimer);
      window.clearTimeout(pressTimer);
      clickTimer = window.setTimeout(() => {
        setOn(stage, step.on || prior);
        const target = stage.querySelector(`[data-hit="${step.hit}"]`);
        if (step.click && cursor) {
          cursor.classList.add("is-clicking");
          if (target) target.classList.add("is-press");
          pressTimer = window.setTimeout(() => {
            cursor.classList.remove("is-clicking");
            if (target) target.classList.remove("is-press");
          }, 180);
        }
      }, 720);
    }

    window.requestAnimationFrame(() => applyBeat());
    window.setInterval(() => {
      beat = (beat + 1) % config.beats.length;
      applyBeat();
    }, config.period);
    if (typeof ResizeObserver !== "undefined") {
      const observer = new ResizeObserver(() => moveCursor(stage, config.beats[beat].hit));
      observer.observe(stage);
    }
  }

  function init() {
    const reduce = reducedMotion();
    document.querySelectorAll("[data-theater-stage]").forEach((stage) => {
      const name = stage.getAttribute("data-theater-stage");
      const config = SCRIPTS[name];
      if (config) playStage(stage, config, reduce);
    });
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }

  document.addEventListener("strata:langchange", () => {
    document.querySelectorAll("[data-theater-stage]").forEach((stage) => {
      const status = stage.querySelector("[data-status-idle]");
      if (status && status.classList.contains("is-on")) {
        /* keep current verb after i18n overwrites */
      }
    });
  });
})();
