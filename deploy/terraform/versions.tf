terraform {
  required_version = ">= 1.11"

  required_providers {
    hcloud = {
      source  = "hetznercloud/hcloud"
      version = "~> 1.50"
    }
  }

  # State lives in the private R2 bucket dswt-terraform. "s3" names the API format that
  # R2 speaks; nothing here uses AWS. dswt.py supplies the endpoint and credentials.
  backend "s3" {
    bucket                      = "dswt-terraform"
    key                         = "dswt.tfstate"
    region                      = "auto"
    use_lockfile                = true
    use_path_style              = true
    skip_credentials_validation = true
    skip_metadata_api_check     = true
    skip_region_validation      = true
    skip_requesting_account_id  = true
    skip_s3_checksum            = true
  }
}

provider "hcloud" {}
