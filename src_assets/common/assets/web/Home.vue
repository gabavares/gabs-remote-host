<template>
  <Navbar></Navbar>
  <div id="content" class="container">
    <h1 class="my-4">{{ $t('index.welcome') }}</h1>
    <p>{{ $t('index.description') }}</p>

    <!-- Fatal Errors Alert -->
    <div class="alert alert-danger my-4" v-if="fancyLogs.some(x => x.level === 'Fatal')">
      <div>
        <div class="d-flex align-items-center mb-3">
          <alert-circle :size="32" class="icon-lg me-3"></alert-circle>
          <div v-html="$t('index.startup_errors')"></div>
        </div>
        <ul class="mb-3">
          <li v-for="v in fancyLogs.filter(x => x.level === 'Fatal')" :key="`${v.timestamp}-${v.value}`">{{v.value}}</li>
        </ul>
        <RouterLink class="btn btn-danger" to="/troubleshooting#logs">
          <file-text :size="18" class="icon"></file-text>
          View Logs
        </RouterLink>
      </div>
    </div>

    <div class="alert alert-warning my-4" v-if="missingPermissions.length">
      <div class="d-flex align-items-center gap-3 mb-2">
        <alert-triangle :size="24" class="icon"></alert-triangle>
        <strong>{{ $t('index.permissions_missing_title') }}</strong>
      </div>
      <p>{{ $t('index.permissions_missing_desc') }}</p>
      <RouterLink class="btn btn-warning" to="/troubleshooting#permissions">
        {{ $t('index.permissions_review') }}
      </RouterLink>
    </div>

    <!-- Virtual gamepad broker status -->
    <div class="alert my-4" :class="virtualInputNotice.alertClass" v-if="virtualInputNotice">
      <div>
        <div class="d-flex align-items-center mb-3">
          <alert-triangle v-if="virtualInputNotice.warning" :size="32" class="icon-lg me-3"></alert-triangle>
          <info v-else :size="32" class="icon-lg me-3"></info>
          <div>
            <p class="mb-1"><strong>{{ $t(virtualInputNotice.title) }}</strong></p>
            <p v-for="message in virtualInputNotice.messages" :key="message.key" class="mb-1">
              {{ $t(message.key, message.params || {}) }}
            </p>
          </div>
        </div>
        <RouterLink class="btn" :class="virtualInputNotice.buttonClass" :to="virtualInputNotice.to">
          <gamepad-2 v-if="virtualInputNotice.chooseDriver" :size="18" class="icon"></gamepad-2>
          <wrench v-else :size="18" class="icon"></wrench>
          {{ $t(virtualInputNotice.action) }}
        </RouterLink>
      </div>
    </div>

    <!-- Gabs Remote: versão e atualização automática (decisão 0006) -->
    <div class="card my-4">
      <div class="card-body">
        <h2>{{ $t('gabs_update.title', { version: update.installed || (version && version.version) || '' }) }}</h2>
        <p class="mb-2">
          <strong>{{ $t('gabs_update.state') }}:</strong>
          {{ $t('gabs_update.state_' + (update.state || 'idle')) }}
        </p>
        <div class="alert alert-danger my-3" v-if="update.state === 'error' && update.error">
          <alert-circle :size="18" class="icon"></alert-circle>
          {{ update.error }}
        </div>
        <div class="alert alert-warning my-3" v-if="updateAvailable">
          <h5 class="mb-2">{{ $t('gabs_update.available', { version: update.latest }) }}</h5>
          <p class="mb-2" v-if="update.state === 'ready'">{{ $t('gabs_update.ready_desc') }}</p>
          <div class="release-notes" style="white-space: pre-wrap">{{ update.notes }}</div>
        </div>
        <div class="d-flex gap-2 flex-wrap">
          <button class="btn btn-primary" :disabled="updateBusy || update.state === 'checking' || update.state === 'installing'" @click="checkUpdate">
            <download :size="18" class="icon"></download>
            {{ $t('gabs_update.check') }}
          </button>
          <button class="btn btn-success" :disabled="updateBusy || update.state !== 'ready'" @click="installUpdate">
            <package :size="18" class="icon"></package>
            {{ $t('gabs_update.install_now') }}
          </button>
        </div>
        <div class="alert alert-info my-3" v-if="updateMessage">{{ updateMessage }}</div>
      </div>
    </div>

    <!-- Resources -->
    <div class="my-4">
      <Resource-Card :installed-version-not-stable="installedVersionNotStable"></Resource-Card>
    </div>
  </div>
</template>

<script>
  import { marked } from 'marked'
  import Navbar from './Navbar.vue'
  import ResourceCard from './ResourceCard.vue'
  import SunshineVersion from './sunshine_version'
  import { apiFetch } from './fetch_utils'
  import {
    AlertCircle,
    AlertTriangle,
    FileText,
    Wrench,
    Package,
    Info,
    CheckCircle,
    Download,
    Gamepad2
  } from '@lucide/vue'

  // Configure marked to allow HTML
  marked.setOptions({
    breaks: true,
    gfm: true,
    headerIds: true,
    mangle: false,
    sanitize: false
  });

  console.log("Hello, Sunshine!")
  export default {
    components: {
      Navbar,
      ResourceCard,
      AlertCircle,
      AlertTriangle,
      FileText,
      Wrench,
      Package,
      Info,
      CheckCircle,
      Download,
      Gamepad2
    },
    data() {
      return {
        version: null,
        githubVersion: null,
        notifyPreReleases: false,
        preReleaseVersion: null,
        loading: true,
        logs: null,
        platform: "",
        controllerEnabled: false,
        gamepadDriver: '',
        virtualhid: null,
        virtualhidLicense: null,
        vigembus: null,
        permissions: [],
        // Gabs Remote: updater do host
        update: { installed: '', latest: '', notes: '', state: 'idle', error: '' },
        updateBusy: false,
        updateMessage: '',
      }
    },
    async created() {
      try {
        let config = await fetch("./api/config").then((r) => r.json());
        this.notifyPreReleases = config.notify_pre_releases;
        this.platform = config.platform;
        this.controllerEnabled = config.controller !== "disabled";
        this.gamepadDriver = config.gamepad_driver || '';
        this.version = new SunshineVersion(null, config.version);
        try {
          const response = await fetch('./api/permissions');
          this.permissions = (await response.json()).permissions || [];
        } catch (e) {
          console.error('Failed to fetch permission status:', e);
        }
        console.log("Version: ", this.version.version)
        // Gabs Remote: sem consulta ao GitHub do LizardByte; a versão vem do updater do host (decisão 0006)
        await this.loadUpdate();

        // Read the broker version on both platforms to identify development builds.
        if (this.platform === 'windows' || this.platform === 'macos') {
          try {
            const virtualInputStatus = await fetch("./api/virtual-input/status").then((r) => r.json());
            this.virtualhid = virtualInputStatus.virtualhid;
            if (this.platform === 'windows') {
              this.vigembus = virtualInputStatus.vigembus;
            }
          } catch (e) {
            console.error("Failed to fetch virtual input driver status:", e);
          }
        }
        if (this.platform === 'windows' || this.platform === 'macos') {
          try {
            this.virtualhidLicense = await fetch("./api/virtual-input/license").then((r) => r.json());
          } catch (e) {
            console.error("Failed to fetch Virtual HID Broker license status:", e);
          }
        }
      } catch (e) {
        console.error(e);
      }
      try {
        this.logs = (await fetch("./api/logs").then(r => r.text()))
      } catch (e) {
        console.error(e);
      }
      this.loading = false;
    },
    computed: {
      /** Gabs Remote: uma versão mais nova foi encontrada e ainda não foi instalada. */
      updateAvailable() {
        return !!this.update.latest && this.update.latest !== this.update.installed &&
          ['downloading', 'ready', 'installing'].includes(this.update.state);
      },
      /** Return required permissions that the current platform can check and has not granted. */
      missingPermissions() {
        return this.permissions.filter(permission => permission.required && permission.verifiable && permission.status !== 'granted');
      },
      /**
       * Build the virtual-input message shown on the home page.
       * Warn about broker or gamepad driver issues when virtual gamepads are enabled.
       */
      virtualInputNotice() {
        if (!this.controllerEnabled || this.gamepadDriver === 'none') {
          return null;
        }

        if (this.platform === 'macos') {
          return this.buildMacosVirtualInputNotice();
        }

        if (this.platform !== 'windows' || !this.virtualhid || !this.vigembus) {
          return null;
        }

        const vigembusUsable = this.vigembus.installed && this.vigembus.version_compatible;

        if (!this.gamepadDriver) {
          return this.buildVirtualInputNotice(false, 'index.gamepad_driver_choice_title', [{
            key: 'index.gamepad_driver_choice_desc',
          }], {
            action: 'index.choose_gamepad_driver',
            chooseDriver: true,
            to: '/config#gamepad_driver',
          });
        }

        if (this.gamepadDriver === 'vigembus') {
          return this.buildVigembusNotice(vigembusUsable);
        }

        if (this.virtualhid.installed) {
          return this.buildInstalledVirtualhidNotice(vigembusUsable);
        }

        if (this.gamepadDriver === 'virtualhid') {
          return this.buildVirtualInputNotice(true, 'index.virtualhid_broker_unavailable_title', [{ key: 'index.virtualhid_required_desc' }]);
        }

        if (this.controllerEnabled && !vigembusUsable) {
          return this.buildVirtualInputNotice(true, 'index.virtual_input_unavailable_title', [{ key: 'index.virtual_input_unavailable_desc' }]);
        }

        return this.buildVirtualInputNotice(
          false,
          'index.virtualhid_optional_title',
          [{ key: this.controllerEnabled ? 'index.virtualhid_optional_vigembus_desc' : 'index.virtualhid_optional_desc' }],
        );
      },
      installedVersionNotStable() {
        if (!this.githubVersion || !this.version) {
          return false;
        }
        return this.version.isGreater(this.githubVersion);
      },
      stableBuildAvailable() {
        if (!this.githubVersion || !this.version) {
          return false;
        }
        return this.githubVersion.isGreater(this.version);
      },
      preReleaseBuildAvailable() {
        if (!this.preReleaseVersion || !this.githubVersion || !this.version) {
          return false;
        }
        return this.preReleaseVersion.isGreater(this.version) && this.preReleaseVersion.isGreater(this.githubVersion);
      },
      buildVersionIsDirty() {
        return this.version.version?.split(".").length === 5 &&
          this.version.version.includes("dirty")
      },
      /** Parse the text errors, calculating the text, the timestamp and the level */
      fancyLogs() {
        if (!this.logs) return [];
        let regex = /(\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}]):\s/g;
        let rawLogLines = (this.logs.split(regex)).splice(1);
        let logLines = []
        for (let i = 0; i < rawLogLines.length; i += 2) {
          logLines.push({ timestamp: rawLogLines[i], level: rawLogLines[i + 1].split(":")[0], value: rawLogLines[i + 1] });
        }
        return logLines;
      }
    },
    methods: {
      /** Gabs Remote: estado do updater do host. */
      async loadUpdate() {
        try {
          this.update = await fetch("./api/gabs/update").then((r) => r.json());
        } catch (e) {
          console.error("Failed to fetch the update status:", e);
        }
      },
      /** Gabs Remote: procura atualização agora e acompanha o estado por até 1 minuto. */
      async checkUpdate() {
        this.updateBusy = true;
        this.updateMessage = '';
        try {
          await apiFetch("./api/gabs/update/check", { method: "POST", headers: { "Content-Type": "application/json" } });
          for (let i = 0; i < 30; i++) {
            await new Promise((resolve) => setTimeout(resolve, 2000));
            await this.loadUpdate();
            if (!['idle', 'checking', 'downloading'].includes(this.update.state)) {
              break;
            }
          }
        } catch (e) {
          console.error(e);
        }
        this.updateBusy = false;
      },
      /** Gabs Remote: instala agora (o host só aceita sem sessão). */
      async installUpdate() {
        this.updateBusy = true;
        try {
          const r = await apiFetch("./api/gabs/update/install", { method: "POST", headers: { "Content-Type": "application/json" } }).then((r) => r.json());
          this.updateMessage = r.status ? this.$t('gabs_update.installing_desc') : r.error;
          await this.loadUpdate();
        } catch (e) {
          console.error(e);
        }
        this.updateBusy = false;
      },
      /**
       * Build the macOS broker notice, prioritizing license and service warnings.
       *
       * @returns {object|null} Warning or development-build notice, when applicable.
       */
      buildMacosVirtualInputNotice() {
        if (this.virtualhidLicense && !this.virtualhidLicense.licensed) {
          return this.virtualhidLicense.service_available
            ? this.buildVirtualInputNotice(true, 'index.virtualhid_macos_license_title', [{ key: 'index.virtualhid_macos_license_desc' }])
            : this.buildVirtualInputNotice(true, 'index.virtualhid_broker_unavailable_title', [{ key: 'index.virtualhid_macos_broker_desc' }]);
        }
        return this.virtualhid?.development_version
          ? this.buildVirtualInputNotice(false, 'index.virtualhid_development_title', [{ key: 'index.virtualhid_development_desc' }])
          : null;
      },
      /**
       * Build a home-page notice for the current virtual-input state.
       *
       * @param {boolean} warning Whether the notice represents an actionable warning.
       * @param {string} title Localization key for the notice title.
       * @param {object[]} messages Localized message descriptors.
       * @param {object} options Optional action and destination overrides.
       * @returns {object} Notice data consumed by the template.
       */
      buildVirtualInputNotice(warning, title, messages, options = {}) {
        return {
          action: options.action || 'index.review_virtual_input',
          alertClass: warning ? 'alert-warning' : 'alert-info',
          buttonClass: warning ? 'btn-warning' : 'btn-info',
          chooseDriver: options.chooseDriver || false,
          to: options.to || '/troubleshooting#virtualhid',
          messages,
          title,
          warning,
        };
      },
      /**
       * Build the notice for an explicitly selected ViGEmBus backend.
       *
       * @param {boolean} vigembusUsable Whether ViGEmBus is installed and compatible.
       * @returns {object|null} Warning data, or no notice when ViGEmBus is usable.
       */
      buildVigembusNotice(vigembusUsable) {
        if (!this.controllerEnabled || vigembusUsable) {
          return null;
        }
        return this.buildVirtualInputNotice(true, 'index.vigembus_required_title', [{
          key: this.vigembus.installed ? 'index.vigembus_outdated_desc' : 'index.vigembus_not_installed_desc',
          params: { version: this.vigembus.version, supported_versions: this.vigembus.supported_versions },
        }]);
      },
      /**
       * Build the notice for an installed Virtual HID Driver.
       *
       * @param {boolean} vigembusUsable Whether ViGEmBus is available as a fallback.
       * @returns {object|null} Warning or informational data, or no notice when fully usable.
       */
      buildInstalledVirtualhidNotice(vigembusUsable) {
        const messages = [];
        if (!this.virtualhid.version_compatible && !this.virtualhid.development_version) {
          messages.push({
            key: 'index.virtualhid_outdated_desc',
            params: { version: this.virtualhid.version, supported_versions: this.virtualhid.supported_versions },
          });
        }
        if (this.virtualhidLicense && !this.virtualhidLicense.licensed) {
          const licenseMessageKey = this.gamepadDriver === 'all' && vigembusUsable ?
            'index.virtualhid_license_invalid_fallback_desc' :
            'index.virtualhid_license_invalid_desc';
          messages.push({ key: licenseMessageKey });
        }
        if (messages.length) {
          return this.buildVirtualInputNotice(true, 'index.virtualhid_attention_title', messages);
        }
        if (this.virtualhid.development_version) {
          return this.buildVirtualInputNotice(false, 'index.virtualhid_development_title', [{ key: 'index.virtualhid_development_desc' }]);
        }
        return null;
      },
      convertMarkdownToHtml(markdown) {
        if (!markdown) return '';
        return marked.parse(markdown);
      }
    }
  }
</script>
