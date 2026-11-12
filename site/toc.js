// Sidebar behaviour: mark the section being read, open its sub-headings, and fold the sidebar
// on narrow screens. Without scripts the sidebar simply lists everything.
(function () {
  var sidebar = document.querySelector(".sidebar");
  if (!sidebar) return;

  var narrow = window.matchMedia("(max-width: 60rem)");
  function fold() {
    if (narrow.matches) sidebar.removeAttribute("open");
    else sidebar.setAttribute("open", "");
  }
  fold();
  if (narrow.addEventListener) narrow.addEventListener("change", fold);

  var links = {};
  sidebar.querySelectorAll("a[href^='#']").forEach(function (a) {
    links[decodeURIComponent(a.getAttribute("href").slice(1))] = a;
  });
  var headings = [];
  document.querySelectorAll("article h2[id], article h3[id]").forEach(function (h) {
    if (links[h.id]) headings.push(h);
  });
  if (!headings.length) return;

  var current = null;
  function select(id) {
    if (id === current) return;
    current = id;
    sidebar.querySelectorAll("a[aria-current]").forEach(function (a) { a.removeAttribute("aria-current"); });
    sidebar.querySelectorAll("li.open").forEach(function (li) { li.classList.remove("open"); });
    var a = links[id];
    a.setAttribute("aria-current", "true");
    var li = a.parentElement;
    li.classList.add("open");
    if (li.parentElement.parentElement.tagName === "LI") li.parentElement.parentElement.classList.add("open");
    // keep the current entry visible inside the sidebar's own scroll area
    var box = sidebar.getBoundingClientRect();
    var r = a.getBoundingClientRect();
    if (!narrow.matches && (r.top < box.top + 8 || r.bottom > box.bottom - 8)) {
      sidebar.scrollTop += r.top - box.top - box.height / 3;
    }
  }

  var ticking = false;
  function update() {
    ticking = false;
    var line = 96; // just below the sticky top bar
    var pick = headings[0];
    for (var i = 0; i < headings.length; i++) {
      if (headings[i].getBoundingClientRect().top <= line) pick = headings[i];
      else break;
    }
    select(pick.id);
  }
  window.addEventListener("scroll", function () {
    if (!ticking) { ticking = true; window.requestAnimationFrame(update); }
  }, { passive: true });
  update();
})();
