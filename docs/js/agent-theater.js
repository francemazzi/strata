(function () {
  const CURSORS = [
    { id: "analisi", x: [18, 62, 22, 70], y: [28, 22, 68, 62] },
    { id: "export", x: [72, 28, 74, 24], y: [62, 70, 26, 32] },
  ];

  function reducedMotion() {
    return window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  }

  function setActive(root, step) {
    root.querySelectorAll("[data-theater-active]").forEach((node) => {
      const values = node.getAttribute("data-theater-active").split(" ").map(Number);
      node.classList.toggle("is-active", values.includes(step));
      node.classList.toggle("is-dim", !values.includes(step) && node.hasAttribute("data-theater-dim"));
    });
    if (reducedMotion()) return;
    CURSORS.forEach((cursor, index) => {
      const el = root.querySelectorAll(".feature-cursor")[index];
      if (!el) return;
      el.style.left = `${cursor.x[step]}%`;
      el.style.top = `${cursor.y[step]}%`;
    });
  }

  function init() {
    const root = document.querySelector("[data-agent-theater]");
    if (!root) return;
    const cursors = root.querySelectorAll(".feature-cursor");
    if (reducedMotion()) {
      cursors.forEach((el) => el.remove());
      setActive(root, 3);
      return;
    }
    let step = 0;
    setActive(root, step);
    window.setInterval(() => {
      step = (step + 1) % 4;
      setActive(root, step);
    }, 2800);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
