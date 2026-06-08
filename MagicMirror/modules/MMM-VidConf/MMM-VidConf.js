Module.register("MMM-VidConf", {

    defaults: {
        title: "Video Conference"
    },

    getStyles() {
        return [
            "MMM-VidConf.css",
            "font-awesome.css"
        ];
    },

    getDom() {

        const wrapper = document.createElement("div");
        wrapper.className = "vidConfWrapper";

        const card = document.createElement("div");
        card.className = "conferenceCard";

        const header = document.createElement("div");
        header.className = "conferenceHeader";
        header.innerHTML = this.config.title;

        const body = document.createElement("div");
        body.className = "conferenceBody";

        const icon = document.createElement("div");
        icon.className = "conferenceIcon";

        icon.innerHTML =
            `<i class="fas fa-regular fa-user"></i>`;

        body.appendChild(icon);

        card.appendChild(header);
        card.appendChild(body);

        wrapper.appendChild(card);

        return wrapper;
    }
});