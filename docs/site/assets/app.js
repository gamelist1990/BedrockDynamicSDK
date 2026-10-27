'use strict';
const sections = [...document.querySelectorAll('main > section')];
const links = [...document.querySelectorAll('nav a')];
const search = document.querySelector('#search');
search.addEventListener('input', () => {
  const query = search.value.trim().toLocaleLowerCase();
  let count = 0;
  for (const section of sections) {
    const match = !query || `${section.dataset.title} ${section.textContent}`.toLocaleLowerCase().includes(query);
    section.hidden = !match;
    const link = links.find(a => a.hash === `#${section.id}`);
    if (link) link.hidden = !match;
    count += Number(match);
  }
  document.querySelector('#no-results').hidden = count > 0;
});
for (const pre of document.querySelectorAll('pre')) {
  const button = document.createElement('button');
  button.type = 'button'; button.className = 'copy'; button.textContent = 'コピー';
  button.setAttribute('aria-label', 'コードをコピー');
  button.addEventListener('click', async () => {
    try {
      await navigator.clipboard.writeText(pre.querySelector('code').textContent);
      button.textContent = 'コピーしました';
      setTimeout(() => { button.textContent = 'コピー'; }, 1600);
    } catch { button.textContent = '手動で選択してください'; }
  });
  pre.append(button);
}
if ('IntersectionObserver' in globalThis) {
  const observer = new IntersectionObserver(entries => {
    for (const entry of entries) if (entry.isIntersecting) {
      for (const link of links) {
        const active = link.hash === `#${entry.target.id}`;
        link.classList.toggle('active', active);
        if (active) link.setAttribute('aria-current', 'location');
        else link.removeAttribute('aria-current');
      }
    }
  }, {rootMargin: '-5% 0px -65% 0px'});
  sections.forEach(section => observer.observe(section));
}
