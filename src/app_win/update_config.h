#ifndef UPDATE_CONFIG_H
#define UPDATE_CONFIG_H

/* Where updates come from and who may sign them. The version itself lives in
   Directory.Build.props; this file holds only the update channel's identity.
   A fork must change the repository and the signer pins, or it will refuse its
   own installers. */

#define ASB_UPDATE_API_URL \
    L"https://api.github.com/repos/lunaboppin/appsandbox/releases?per_page=30"
#define ASB_UPDATE_ASSET_PREFIX \
    L"https://github.com/lunaboppin/appsandbox/releases/download/"
#define ASB_UPDATE_RELEASE_PREFIX \
    L"https://github.com/lunaboppin/appsandbox/releases/"

/* Must match AppId in tools\installer\AppSandbox.iss (Inno writes its uninstall
   key under "<AppId>_is1"). Never change it: it is what makes an upgrade an upgrade. */
#define ASB_INSTALLER_APP_ID L"{7D3F2A64-9B1E-4C58-A0D7-3E6C1F82B954}"

/* SHA-256 of each certificate allowed to sign an update installer. An installer
   signed by anything else is refused, even by a certificate Windows trusts.
   To rotate a certificate, ship one release signed by the OLD certificate that
   adds the NEW one here.
     18A05C56... CN=WDKTestCert Luna AppSandbox (test-signed releases) */
#define ASB_UPDATE_SIGNER_PINS { \
    L"18A05C560707077AF685CE0DA874F2820E27724B2302F4250EA5A80BBB903032", \
}

#endif /* UPDATE_CONFIG_H */
