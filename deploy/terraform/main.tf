# The ROM builds in the server's IPv4 address, so losing it means new patches for every
# player. Terraform adopts these existing resources and must never replace them.
import {
  to = hcloud_primary_ip.ipv4
  id = "160335882"
}

import {
  to = hcloud_primary_ip.ipv6
  id = "160335883"
}

import {
  to = hcloud_firewall.dswt
  id = "11973775"
}

import {
  to = hcloud_server.dswt
  id = "174545294"
}

resource "hcloud_primary_ip" "ipv4" {
  name              = "primary_ip-160335882"
  type              = "ipv4"
  location          = "ash"
  auto_delete       = false
  delete_protection = true

  lifecycle {
    prevent_destroy = true
    ignore_changes  = [name, assignee_id, assignee_type, location]
  }
}

resource "hcloud_primary_ip" "ipv6" {
  name              = "primary_ip-160335883"
  type              = "ipv6"
  location          = "ash"
  auto_delete       = false
  delete_protection = true

  lifecycle {
    prevent_destroy = true
    ignore_changes  = [name, assignee_id, assignee_type, location]
  }
}

resource "hcloud_firewall" "dswt" {
  name = "DSWT Firewall"

  rule {
    description = "General Game"
    direction   = "in"
    protocol    = "tcp"
    port        = "443"
    source_ips  = ["0.0.0.0/0", "::/0"]
  }

  rule {
    description = "SSH"
    direction   = "in"
    protocol    = "tcp"
    port        = "22"
    source_ips  = var.admin_cidrs
  }

  lifecycle {
    prevent_destroy = true
  }
}

resource "hcloud_server" "dswt" {
  name               = "DSWT"
  server_type        = "cpx11"
  location           = "ash"
  image              = "ubuntu-26.04"
  backups            = true
  firewall_ids       = [hcloud_firewall.dswt.id]
  delete_protection  = true
  rebuild_protection = true

  public_net {
    ipv4 = hcloud_primary_ip.ipv4.id
    ipv6 = hcloud_primary_ip.ipv6.id
  }

  lifecycle {
    prevent_destroy = true
    # Creation-time settings: changing any of these would rebuild the server.
    ignore_changes = [image, ssh_keys, user_data, public_net]
  }
}
