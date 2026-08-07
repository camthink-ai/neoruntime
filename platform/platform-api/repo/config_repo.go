package repo

import (
	"time"

	"aipc/platform/platform-api/model"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"
)

// ConfigRepo persists Config Controller state: desired ConfigItem rows, the
// append-only ConfigRevision history, and ConfigApplyJob lifecycle records.
// It mirrors SettingRepo's style (UPSERT via clause.OnConflict) but is keyed
// on the composite (domain, key) and adds revision-history + job methods.
//
// All methods are thin DB accessors; the Config Manager (config package) owns
// the apply state machine and calls these in the right order.
type ConfigRepo struct {
	db *gorm.DB
}

// NewConfigRepo creates a new ConfigRepo.
func NewConfigRepo(db *gorm.DB) *ConfigRepo {
	return &ConfigRepo{db: db}
}

// Get returns the ConfigItem for (domain, key). found is false (with a nil
// item and nil error) when no row exists.
func (r *ConfigRepo) Get(domain, key string) (item *model.ConfigItem, found bool, err error) {
	var it model.ConfigItem
	err = r.db.Where("domain = ? AND `key` = ?", domain, key).First(&it).Error
	if err != nil {
		if err == gorm.ErrRecordNotFound {
			return nil, false, nil
		}
		return nil, false, err
	}
	return &it, true, nil
}

// Upsert creates or updates the desired-state ConfigItem. The (domain, key)
// conflict target is updated in place; Revision is written from the passed
// item (the manager sets it to the value returned by AppendRevision).
func (r *ConfigRepo) Upsert(item *model.ConfigItem) error {
	return r.db.Clauses(clause.OnConflict{
		Columns: []clause.Column{{Name: "domain"}, {Name: "key"}},
		DoUpdates: clause.AssignmentColumns([]string{
			"value_json", "schema_version", "revision", "updated_at", "updated_by",
		}),
	}).Create(item).Error
}

// GetAll returns every ConfigItem in a domain.
func (r *ConfigRepo) GetAll(domain string) ([]model.ConfigItem, error) {
	var items []model.ConfigItem
	if err := r.db.Where("domain = ?", domain).Find(&items).Error; err != nil {
		return nil, err
	}
	return items, nil
}

// Delete removes the desired-state ConfigItem for (domain, key). It does not
// touch revision history (history is append-only).
func (r *ConfigRepo) Delete(domain, key string) error {
	return r.db.Where("domain = ? AND `key` = ?", domain, key).
		Delete(&model.ConfigItem{}).Error
}

// ListRevisions returns the revision history for (domain, key) in ascending
// revision order.
func (r *ConfigRepo) ListRevisions(domain, key string) ([]model.ConfigRevision, error) {
	var revs []model.ConfigRevision
	if err := r.db.Where("domain = ? AND `key` = ?", domain, key).
		Order("revision ASC").Find(&revs).Error; err != nil {
		return nil, err
	}
	return revs, nil
}

// AppendRevision appends a new history row for (domain, key), assigning the
// next revision number (max(existing)+1, or 1 if none) inside a transaction
// so concurrent Appends cannot collide. It returns the assigned revision.
//
// The caller supplies the value/reason/createdBy; CreatedAt is set here.
func (r *ConfigRepo) AppendRevision(domain, key, valueJSON, reason, createdBy string) (int, error) {
	var assigned int
	err := r.db.Transaction(func(tx *gorm.DB) error {
		var maxRev int
		if err := tx.Model(&model.ConfigRevision{}).
			Where("domain = ? AND `key` = ?", domain, key).
			Select("COALESCE(MAX(revision), 0)").Scan(&maxRev).Error; err != nil {
			return err
		}
		assigned = maxRev + 1
		rev := model.ConfigRevision{
			Domain:    domain,
			Key:       key,
			Revision:  assigned,
			ValueJSON: valueJSON,
			Reason:    reason,
			CreatedAt: time.Now(),
			CreatedBy: createdBy,
		}
		return tx.Create(&rev).Error
	})
	if err != nil {
		return 0, err
	}
	return assigned, nil
}

// CreateJob inserts a new ConfigApplyJob. The ID is caller-supplied (UUID) so
// the handler can return it before Apply completes.
func (r *ConfigRepo) CreateJob(job *model.ConfigApplyJob) error {
	return r.db.Create(job).Error
}

// FinishJob marks a job terminal: sets Status, Error, ToRevision, and
// FinishedAt to now.
func (r *ConfigRepo) FinishJob(id, status, errMsg string, toRevision int) error {
	now := time.Now()
	return r.db.Model(&model.ConfigApplyJob{}).Where("id = ?", id).
		Updates(map[string]any{
			"status":      status,
			"error":       errMsg,
			"to_revision": toRevision,
			"finished_at": &now,
		}).Error
}

// GetJob returns a single ConfigApplyJob by ID. A missing job yields
// gorm.ErrRecordNotFound (the handler maps this to a not-found response).
func (r *ConfigRepo) GetJob(id string) (*model.ConfigApplyJob, error) {
	var j model.ConfigApplyJob
	if err := r.db.Where("id = ?", id).First(&j).Error; err != nil {
		return nil, err
	}
	return &j, nil
}

// ListJobs returns ConfigApplyJob rows newest-first. domain=="" selects all
// domains; limit<=0 or limit>maxJobListLimit is clamped to maxJobListLimit.
// Newest-first ordering (started_at DESC, id DESC) gives operators the most
// recent apply attempts at the top of the audit view.
func (r *ConfigRepo) ListJobs(domain string, limit int) ([]model.ConfigApplyJob, error) {
	if limit <= 0 || limit > maxJobListLimit {
		limit = maxJobListLimit
	}
	q := r.db.Model(&model.ConfigApplyJob{}).Order("started_at DESC, id DESC").Limit(limit)
	if domain != "" {
		q = q.Where("domain = ?", domain)
	}
	var jobs []model.ConfigApplyJob
	if err := q.Find(&jobs).Error; err != nil {
		return nil, err
	}
	return jobs, nil
}

// maxJobListLimit caps ListJobs so an unbounded query cannot return the whole
// table. Matches the audit endpoint's default page size (100).
const maxJobListLimit = 100
