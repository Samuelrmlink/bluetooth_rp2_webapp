// Pictures in the editor: a .png .jpg .gif .webp .ico or .svg (and .svg.gz)
// opens as the picture, with its pixel size, file size and flash used, so
// the screensaver's JPEGs and the UI's icons can be told apart in the file
// list. Bitmaps are read-only (replace one by uploading); an SVG is text as
// well: the titlebar's Text / Preview button switches (editor.js).

export class ImagePreview {
  constructor(surface) {
    this.el = document.createElement('div');
    this.el.className = 'ed-preview';
    this.el.hidden = true;
    this.el.innerHTML = `
      <div class="ed-pv-bar"><span class="ed-pv-info dim"></span></div>
      <div class="ed-pv-img"><img alt=""></div>`;
    surface.appendChild(this.el);
    this.img = this.el.querySelector('img');
    this.info = this.el.querySelector('.ed-pv-info');
    this.url = null;
  }

  get shown() { return !this.el.hidden; }

  // blob: the picture's bytes, already decompressed (the device serves .gz
  // with Content-Encoding, so fetch unpacks it). note: size / flash line.
  show(blob, { note = '' } = {}) {
    this.hide();
    this.url = URL.createObjectURL(blob);
    this.info.textContent = note;
    this.img.onload = () => {
      const w = this.img.naturalWidth, h = this.img.naturalHeight;
      this.img.classList.toggle('is-small', w > 0 && w < 64 && h < 64);
      this.info.textContent = [w && h ? `${w} × ${h} px` : '', note].filter(Boolean).join(' · ');
    };
    this.img.onerror = () => { this.info.textContent = ['cannot be shown (not a picture this browser reads)', note].filter(Boolean).join(' · '); };
    this.img.src = this.url;
    this.el.hidden = false;
  }

  hide() {
    this.el.hidden = true;
    this.img.removeAttribute('src');
    if (this.url) { URL.revokeObjectURL(this.url); this.url = null; }
  }
}
