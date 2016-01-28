<?php

/**
 * @file
 * This file contains functions to create breadcrumbs on Islandora object pages.
 */

/**
 * Get an array of links to be passed to drupal_set_breadcrumb().
 *
 * This is used for generating the bread-crumbs for the view object page.
 *
 * Each link in the bread-crumbs represents a member of the given objects
 * ancestry which is identified by any of the following RELS-EXT terms
 * (isMemberOf,isMemberOfCollection,isPartOf).
 *
 * @param AbstractObject $object
 *   An object whose ancestry will be mapped to bread-crumbs.
 *
 * @see drupal_set_breadcrumb()
 *
 * @return array
 *   Array of links, starting with most distant ancestor proceeding up to but
 *   not including the given object. For use in the function
 *   drupal_set_breadcrumb().
 */
function islandora_get_breadcrumbs($object) {
  $breadcrumbs = array();
  if (variable_get('islandora_render_drupal_breadcrumbs', TRUE)) {
    $breadcrumbs = islandora_get_breadcrumbs_recursive($object->id, $object->repository);
    array_pop($breadcrumbs);
    $context = 'islandora';
    drupal_alter('islandora_breadcrumbs', $breadcrumbs, $context, $object);
  }
  return $breadcrumbs;
}

/**
 * Builds an array of drupal links for use in breadcrumbs.
 *
 * @todo Make fully recursive...
 *
 * @param string $pid
 *   The object id whose parent will be fetched for the next link.
 * @param FedoraRepository $repository
 *   The fedora repository.
 * @param array $context
 *   An associative array of context for internal use when recursing. Currently
 *   only used to track a single value:
 *   - level: The number of child-parent relationships to follow. Defaults to
 *     10.
 *   -root_object: The object that is being rendered for.
 *
 * @return array
 *   An array of links representing the breadcrumb trail, "root" first.
 */
function islandora_get_breadcrumbs_recursive($pid, FedoraRepository $repository, array &$context = NULL) {
  module_load_include('inc', 'islandora', 'includes/utilities');
  if ($context === NULL) {
    $context['level'] = 10;
  }

  $root = variable_get('islandora_repository_pid', 'islandora:root');
  if ($pid == $root) {
    $title = 'Islandora Repository';
    $mlid = db_select('menu_links', 'ml')
      ->condition('ml.link_path', 'islandora')
      ->fields('ml', array('mlid'))
      ->execute()
      ->fetchField();

    if ($mlid) {
      $link = menu_link_load($mlid);
      $title = (isset($link['title']) ? $link['title'] : $title);
    }
    return array(
      l(t('Home'), '<front>'),
      l($title, 'islandora'),
    );
  }
  else {
    $result = FALSE;
    $object = islandora_object_load($pid);
    $breadcrumb_predicates = islandora_invoke_hook_list(ISLANDORA_BREADCRUMB_FILTER_PREDICATE_HOOK, $object->models, array($object));

    if (FALSE) {
      $query = <<<EOQ
      PREFIX islandora-rels-ext: <http://islandora.ca/ontology/relsext#>
      PREFIX fedora-rels-ext: <info:fedora/fedora-system:def/relations-external#>
      SELECT DISTINCT ?object ?label
      FROM <#ri>
      WHERE {
          <info:fedora/!pid> ?collection_predicate ?object;
          <fedora-model:label> ?label .
          ?object <fedora-model:state> <fedora-model:Active>
          .
          !optionals
          !filters
            }
          !order
EOQ;

      $query_optionals = (array) module_invoke('islandora_xacml_api', 'islandora_basic_collection_get_query_optionals', 'view');
      $query_filters = (array) module_invoke('islandora_xacml_api', 'islandora_basic_collection_get_query_filters');
      $filter_map = function ($filter) {
        return "FILTER($filter)";
      };
      $same_term_map = function ($predicate) {
        return "sameTerm(?collection_predicate, <$predicate>)";
      };
      $collection_predicate_filters = array_map($same_term_map, $breadcrumb_predicates);
      $query_filters[] = implode(' || ', $collection_predicate_filters);
      $query_order = "ORDER BY DESC(?label)";
      $query = format_string($query, array(
        '!optionals' => !empty($query_optionals) ? ('OPTIONAL {{' . implode('} UNION {', $query_optionals) . '}}') : '',
        '!filters' => implode(' ', array_map($filter_map, $query_filters)),
        '!order' => $query_order,
      ));
      $query = format_string($query, array(
        '!pid' => $pid,
      ));
      $results = $repository->ri->sparqlQuery($query, 'unlimited');
      // Format the results into a generic structure.
      if (count($results)) {
        $result = reset($results);
        $label = $result['label']['value'];
        $parent = $result['object']['value'];
      }
    }
    // Use solr.
    else {
      $qp = new IslandoraSolrQueryProcessor();
      $base_fl = array(
        'PID',
        variable_get('islandora_solr_object_label', 'fgs_label_s'),
      );
      $breadcrumb_fls = array();
      foreach ($breadcrumb_predicates as $predicate) {
        // Dumb mapping for now.
        if ($predicate == 'fedora-rels-ext:isMemberOf') {
          $solr = variable_get('islandora_solr_member_of_field', 'RELS_EXT_isMemberOf_uri_ms');
        }
        else {
          $solr = variable_get('islandora_solr_member_of_collection_field', 'RELS_EXT_isMemberOfCollection_uri_ms');
        }
        if (!empty($solr)) {
          $breadcrumb_fls[] = $solr;
        }
      }
      $qp->buildQuery(format_string('PID:"@pid"', array('@pid' => $pid)));
      $qp->solrParams['fl'] = implode(', ', array_merge($base_fl, $breadcrumb_fls));
      $qp->solrLimit = 1;
      $qp->executeQuery(FALSE);
      // Format results into a generic structure.
      if ($qp->islandoraSolrResult['response']['numFound'] > 0) {
        $result = reset($qp->islandoraSolrResult['response']['objects']);
        foreach ($result['solr_doc'] as $field => $values) {
          if (in_array($field, $breadcrumb_fls)) {
            $parent = str_replace('info:fedora/', '', reset($values));
            $label = $result['object_label'];
            break;
          }
        }
      }
    }
  }
  if ($result && $context['level'] > 0) {
    if (empty($label)) {
      $label = t('-');
    }
    $context['level']--;
    return array_merge(islandora_get_breadcrumbs_recursive($parent, $repository, $context), array(
      l($label, "islandora/object/$pid"),
    ));
  }
  else {
    // Add an non-link, as we don't know how to get back to the root, and
    // render the last two links and break (on the next pass).
    return array_merge(
      islandora_get_breadcrumbs_recursive($root, $repository, $context),
      array('...')
    );
  }
}
