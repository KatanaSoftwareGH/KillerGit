// Commit graph lane layout. Input: commits in topo order. Output: per-row lane info for SVG drawing.
const Graph = (() => {
  const COLORS = ['#2f81f7', '#3fb950', '#d29922', '#db61a2', '#a371f7', '#f85149', '#39c5cf', '#e3b341', '#8b949e'];
  const W = 14, H = 26;

  // lanes: array of commit hashes each lane is "waiting for".
  function layout(commits, prev) {
    let lanes = prev ? prev.slice() : [];
    const rows = [];
    for (const c of commits) {
      const before = lanes.slice();
      let col = lanes.indexOf(c.hash);
      if (col === -1) { col = lanes.indexOf(null); if (col === -1) col = lanes.length; }
      lanes = lanes.map(h => (h === c.hash ? null : h));
      lanes[col] = c.parents[0] || null;
      for (const p of c.parents.slice(1)) {
        if (lanes.includes(p)) continue;
        let free = lanes.indexOf(null);
        if (free === -1) free = lanes.length;
        lanes[free] = p;
      }
      while (lanes.length && lanes[lanes.length - 1] === null) lanes.pop();
      rows.push({ hash: c.hash, col, before, after: lanes.slice(), parents: c.parents });
    }
    return { rows, lanes };
  }

  const x = i => i * W + W / 2 + 2;
  const color = i => COLORS[i % COLORS.length];

  function path(x1, y1, x2, y2) {
    if (x1 === x2) return `M${x1} ${y1}L${x2} ${y2}`;
    const my = (y1 + y2) / 2;
    return `M${x1} ${y1}C${x1} ${my} ${x2} ${my} ${x2} ${y2}`;
  }

  function svg(row, width) {
    const mid = H / 2;
    let s = '';
    row.before.forEach((h, i) => {
      if (!h) return;
      if (h === row.hash) s += `<path d="${path(x(i), 0, x(row.col), mid)}" stroke="${color(i)}"/>`;
      else {
        const j = row.after.indexOf(h);
        if (j !== -1) s += `<path d="${path(x(i), 0, x(j), H)}" stroke="${color(j)}"/>`;
      }
    });
    row.parents.forEach(p => {
      const j = row.after.indexOf(p);
      if (j !== -1) s += `<path d="${path(x(row.col), mid, x(j), H)}" stroke="${color(j)}"/>`;
    });
    const merge = row.parents.length > 1;
    s += `<circle cx="${x(row.col)}" cy="${mid}" r="${merge ? 3.5 : 4.5}" fill="${merge ? 'var(--bg)' : color(row.col)}" stroke="${color(row.col)}" stroke-width="2"/>`;
    return `<svg width="${width}" height="${H}" fill="none" stroke-width="2">${s}</svg>`;
  }

  const widthFor = rows => Math.min(Math.max(...rows.map(r => Math.max(r.before.length, r.after.length, r.col + 1)), 1) * W + 8, 260);

  return { layout, svg, widthFor, H };
})();
