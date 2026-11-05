variable "admin_cidrs" {
  description = "Addresses allowed to reach SSH, from DSWT_ADMIN_CIDRS in the ignored .env."
  type        = list(string)
  sensitive   = true

  validation {
    condition     = length(var.admin_cidrs) > 0 && !contains(var.admin_cidrs, "0.0.0.0/0") && !contains(var.admin_cidrs, "::/0")
    error_message = "List your own addresses. SSH must not be open to the internet."
  }
}
